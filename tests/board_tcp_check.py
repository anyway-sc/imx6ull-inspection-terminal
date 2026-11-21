"""由用户在 Windows 手动运行的真实板端 TCP 检查；不调用 ADB 或启停服务。"""
import argparse
from datetime import datetime
from hashlib import sha256
from io import BytesIO
import json
from pathlib import Path
import socket
import sys
import time


class Connection:
    def __init__(self, host, port):
        self.socket = socket.create_connection((host, port), timeout=10)
        self.socket.settimeout(20)
        self.reader = self.socket.makefile("rb")

    def close(self):
        self.reader.close()
        self.socket.close()

    def send(self, command):
        self.socket.sendall(command.encode("ascii"))

    def receive(self):
        header = self.reader.readline(129)
        if not header.endswith(b"\n") or len(header) > 128:
            raise ValueError(f"回复头不完整或超长：{header!r}")
        fields = header.decode("ascii").strip().split()
        if not fields:
            raise ValueError("收到空回复")
        result = {"line": header.decode("ascii").rstrip(), "fields": fields}
        if fields[0] == "JPEG":
            if len(fields) != 3:
                raise ValueError(f"JPEG 头格式不正确：{header!r}")
            size = int(fields[2])
            if not 2 <= size <= 4 * 1024 * 1024:
                raise ValueError(f"JPEG 长度超出协议范围：{size}")
            data = self.reader.read(size)
            if len(data) != size or not data.startswith(b"\xff\xd8"):
                raise ValueError("JPEG 载荷不完整或起始字节错误")
            result["image"] = data
        return result

    def request(self, command, expected_id=None):
        self.send(command + "\n")
        result = self.receive()
        if expected_id is not None and (
                len(result["fields"]) < 2 or result["fields"][1] != str(expected_id)):
            raise ValueError(f"回复编号不匹配：{result['line']}")
        return result


def check_image(reply, output, name):
    if "image" not in reply:
        raise ValueError(f"未收到 JPEG：{reply['line']}")
    data = reply["image"]
    destination = output / name
    destination.write_bytes(data)
    result = {"file": str(destination), "bytes": len(data), "sha256": sha256(data).hexdigest()}
    try:
        from PIL import Image
    except ImportError:
        result["decoded"] = False
        result["note"] = "仅确认载荷长度及 JPEG 起始字节；当前 Python 无 Pillow，未验证解码。"
    else:
        with Image.open(BytesIO(data)) as image:
            result["format"] = image.format
            result["size"] = list(image.size)
            if image.format != "JPEG" or image.size != (640, 480):
                raise ValueError(f"图像格式或尺寸不符合预期：{image.format} {image.size}")
            image.load()
            result["decoded"] = True
    return result


def require_status(reply, request_id, detail):
    expected = ["OK", str(request_id), detail]
    if reply["fields"] != expected:
        raise ValueError(f"预期 {' '.join(expected)}，实际 {reply['line']}")


def smoke(connection, output):
    checks = []
    reply = connection.request("PING")
    if reply["fields"] != ["PONG"]:
        raise ValueError(f"PING 未得到 PONG：{reply['line']}")
    checks.append({"check": "PING", "ok": True})
    print("通过：PING / PONG", flush=True)
    for index, (name, count) in enumerate((("AP3216C", 5), ("ICM20608", 9),
                                         ("DHT11", 4), ("SR04", 4), ("SR501", 3)), 1):
        attempts = []
        success = False
        for attempt in range(4):
            request_id = index * 10 + attempt
            reply = connection.request(f"READ_{name} {request_id}", request_id)
            attempts.append(reply["line"])
            fields = reply["fields"]
            if fields[0] == name:
                if len(fields) != count:
                    raise ValueError(f"{name} 字段数量错误：{reply['line']}")
                values = [int(value) for value in fields[2:]]
                if name == "SR501" and values not in ([0], [1]):
                    raise ValueError(f"SR501 电平错误：{reply['line']}")
                success = True
                break
            if fields != ["ERR", str(request_id), f"{name}_READ"]:
                raise ValueError(f"传感器回复格式错误：{reply['line']}")
            if attempt != 3:
                time.sleep(1)
        checks.append({"check": name, "ok": success, "replies": attempts})
        print(f"{'通过' if success else '待排查'}：{name}，{attempts[-1]}", flush=True)
    reply = connection.request("CAPTURE_JPEG 100", 100)
    capture = check_image(reply, output, "capture.jpg")
    checks.append({"check": "单帧拍照", "ok": True, **capture})
    print(f"通过：拍照载荷，{capture['bytes']} 字节", flush=True)
    require_status(connection.request("PREVIEW_START 101", 101), 101, "PREVIEW_STARTED")
    for request_id in range(102, 105):
        reply = connection.request(f"PREVIEW_FRAME {request_id}", request_id)
        preview = check_image(reply, output, f"preview-{request_id}.jpg")
        checks.append({"check": f"预览帧 {request_id}", "ok": True, **preview})
    require_status(connection.request("PREVIEW_STOP 105", 105), 105, "PREVIEW_STOPPED")
    checks.append({"check": "预览启停", "ok": True})
    print("通过：连续三帧预览及停止", flush=True)
    # 一次发送两条命令，验证文本分帧和请求编号。
    connection.send("READ_SR501 106\nPING\n")
    first, second = connection.receive(), connection.receive()
    if first["fields"][:2] not in (["SR501", "106"], ["ERR", "106"]) or second["fields"] != ["PONG"]:
        raise ValueError(f"合并命令回复错误：{first['line']} / {second['line']}")
    checks.append({"check": "合并命令与文本分帧", "ok": True})
    return checks


def reconnect(host, port, output, cycles):
    checks = []
    for index in range(cycles):
        connection = Connection(host, port)
        try:
            if connection.request("PING")["fields"] != ["PONG"]:
                raise ValueError("重连后 PING 失败")
            require_status(connection.request("PREVIEW_START 1", 1), 1, "PREVIEW_STARTED")
            image = check_image(connection.request("PREVIEW_FRAME 2", 2), output, "reconnect-last.jpg")
            # 不发送 STOP，交由服务的断线路径关闭相机并回收该连接线程。
            checks.append({"check": f"重连 {index + 1}", "ok": True, **image})
        finally:
            connection.close()
        print(f"通过：第 {index + 1}/{cycles} 次连接、取图及直接断线", flush=True)
    return checks


def queue_check(connection, output):
    require_status(connection.request("PREVIEW_START 200", 200), 200, "PREVIEW_STARTED")
    identifiers = set(range(201, 233))
    connection.send("".join(f"PREVIEW_FRAME {number}\n" for number in sorted(identifiers)) + "PING\n")
    busy, frames, pong = 0, 0, False
    while identifiers or not pong:
        reply = connection.receive()
        fields = reply["fields"]
        if fields == ["PONG"]:
            if pong:
                raise ValueError("收到重复 PONG")
            pong = True
            continue
        request_id = int(fields[1])
        if request_id not in identifiers:
            raise ValueError(f"收到重复或未知编号：{reply['line']}")
        identifiers.remove(request_id)
        if fields == ["ERR", str(request_id), "CAMERA_BUSY"]:
            busy += 1
        elif fields[0] == "JPEG":
            check_image(reply, output, "queue-last.jpg")
            frames += 1
        else:
            raise ValueError(f"队列检查发生其他错误：{reply['line']}")
    require_status(connection.request("PREVIEW_STOP 233", 233), 233, "PREVIEW_STOPPED")
    print(f"队列检查：{frames} 帧，{busy} 次 CAMERA_BUSY，所有请求均有完整回复", flush=True)
    return [{"check": "相机突发请求", "ok": True, "frames": frames, "busy": busy,
             "busy_observed": busy > 0, "note": "是否实际触发队列满以本次 busy 数量为准。"}]


def main():
    parser = argparse.ArgumentParser(description="真实板端 TCP 检查，由用户手动执行。")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19001)
    parser.add_argument("--mode", choices=("smoke", "hold", "reconnect", "queue"), default="smoke")
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "results")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 50:
        parser.error("--cycles 必须在 1..50 之间")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"time": datetime.now().astimezone().isoformat(), "host": args.host, "port": args.port,
              "mode": args.mode, "checks": [], "note": "网络数据和图像检查；不证明测量精度或长期稳定性。"}
    connection = None
    exit_code = 0
    try:
        if args.mode == "reconnect":
            report["checks"] = reconnect(args.host, args.port, args.output, args.cycles)
        else:
            connection = Connection(args.host, args.port)
            if args.mode == "hold":
                if connection.request("PING")["fields"] != ["PONG"]:
                    raise ValueError("连接后 PING 失败")
                print("连接已保持。现在可在板端查看主进程 6 个线程和 1 个相机子进程；按 Enter 后断开。", flush=True)
                input()
                report["checks"] = [{"check": "连接保持", "ok": True,
                                      "note": "线程数量需要另行读取板端 /proc，未在此自动检查。"}]
            elif args.mode == "queue":
                report["checks"] = queue_check(connection, args.output)
            else:
                report["checks"] = smoke(connection, args.output)
        if any(not check["ok"] for check in report["checks"]):
            exit_code = 1
    except (OSError, ValueError, EOFError) as error:
        report["error"] = f"{type(error).__name__}: {error}"
        exit_code = 1
        print(f"检查未完成：{error}", flush=True)
    finally:
        if connection is not None:
            connection.close()
        report["exit_code"] = exit_code
        destination = args.output / f"{args.mode}-{datetime.now():%Y%m%d-%H%M%S}.json"
        destination.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"结果记录：{destination}", flush=True)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
