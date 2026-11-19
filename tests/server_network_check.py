"""Linux 集成测试：真实网络、采集线程、相机子进程，设备由 fake_devices.c 替换。"""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

CAMERA_SERVICE = None


def wait_for(predicate, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("等待测试条件超时")


class Connection:
    def __init__(self, small_buffer=False):
        self.socket = socket.socket()
        if small_buffer:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        self.socket.settimeout(8)
        self.socket.connect(("127.0.0.1", 9000))
        self.reader = self.socket.makefile("rb")

    def send(self, data):
        self.socket.sendall(data)

    def receive(self):
        line = self.reader.readline(128)
        assert line.endswith(b"\n"), line
        if line.startswith(b"JPEG "):
            size = int(line.split()[2])
            assert 4 <= size <= 4 * 1024 * 1024
            image = self.reader.read(size)
            assert len(image) == size
            assert image[:4] == b"\xff\xd8\n\0" and image[-2:] == b"\xff\xd9"
            assert image[4:-2] == b"Z" * (size - 6), "JPEG 中混入了文本或丢失字节"
        return line

    def close(self):
        self.reader.close()
        self.socket.close()


class Server:
    def __init__(self, executable, delay=300, size=65536, camera_program=None, extra_env=None):
        self.log = tempfile.TemporaryFile()
        environment = os.environ.copy()
        environment.update(FAKE_CAMERA_DELAY_MS=str(delay), FAKE_JPEG_BYTES=str(size))
        environment['VEHICLE_CAMERA_SERVICE'] = camera_program or CAMERA_SERVICE
        environment.update(extra_env or {})
        self.process = subprocess.Popen([executable, "fake"], env=environment,
                                        stdout=self.log, stderr=subprocess.STDOUT)
        def ready():
            assert self.process.poll() is None, "模拟服务提前退出，9000 端口可能被占用"
            try:
                connection = Connection()
                connection.send(b"PING\n")
                assert connection.receive() == b"PONG\n"
                connection.close()
                return True
            except (ConnectionRefusedError, ConnectionResetError):
                return False
        wait_for(ready)
        wait_for(lambda: self.thread_count() == 6 and self.fd_count() == 6 and not self.children())
        self.idle_fds = self.fd_count()

    def thread_count(self):
        return len(list(Path(f"/proc/{self.process.pid}/task").iterdir()))

    def fd_count(self):
        return len(list(Path(f"/proc/{self.process.pid}/fd").iterdir()))

    def children(self):
        return list(map(int, Path(f"/proc/{self.process.pid}/task/{self.process.pid}/children").read_text().split()))

    def idle(self):
        return self.thread_count() == 6 and self.fd_count() == self.idle_fds and not self.children()

    def stop(self, number=signal.SIGTERM):
        if self.process.poll() is None:
            self.process.send_signal(number)
        try:
            assert self.process.wait(timeout=5) == 0, "信号退出没有完成正常清理"
        except BaseException:
            self.process.kill()
            self.process.wait()
            raise

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self.log.seek(0)
        log = self.log.read().decode("utf-8", errors="replace")
        self.log.close()
        return log


def check_protocol_and_reconnect(executable):
    server = Server(executable)
    try:
        connection = Connection()
        wait_for(lambda: server.thread_count() == 6 and len(server.children()) == 1)
        connection.send(b"READ_SR")
        connection.send(b"04 1\nPING\nREAD_DHT11 2\n")
        assert connection.receive() == b"SR04 1 250 1458\n"
        assert connection.receive() == b"PONG\n"
        assert connection.receive() == b"DHT11 2 600 260\n"
        connection.send(b"READ_AP3216C 4294967295\nREAD_ICM20608 3\nREAD_SR501 4\n")
        fields = connection.receive().split()
        assert fields[:2] == [b"AP3216C", b"4294967295"] and len(fields) == 5
        ir, als, ps = map(int, fields[2:])
        assert als == ir * 2 and ps == ir * 3
        fields = connection.receive().split()
        assert fields[:2] == [b"ICM20608", b"3"] and len(fields) == 9
        gx, gy, gz, ax, ay, az, temperature = map(int, fields[2:])
        assert (gy, gz, ax, ay, az, temperature) == (-gx, gx * 2, -gx * 2, gx * 3, -gx * 3, 25)
        assert connection.receive() == b"SR501 4 1\n"
        for command in (b"READ_SR04 0\n", b"READ_SR04 -1\n", b"READ_SR04 4294967296\n",
                        b"READ_SR04 1\0\n", b"PREVIEW_FRAME 0\n"):
            connection.send(command)
            assert connection.receive() == b"ERR BAD_REQUEST\n"
        connection.send(b"WHAT\n")
        assert connection.receive() == b"ERR UNKNOWN_COMMAND\n"
        connection.send(b"PREVIEW_START 10\n")
        assert connection.receive() == b"OK 10 PREVIEW_STARTED\n"
        connection.send(b"PREVIEW_FRAME 11\nPING\nREAD_SR04 12\n")
        # 慢取图已交给相机子进程，网络线程先响应 PING 和传感器缓存。
        assert connection.receive() == b"PONG\n"
        assert connection.receive() == b"SR04 12 250 1458\n"
        assert connection.receive() == b"JPEG 11 65536\n"
        connection.send(b"PREVIEW_STOP 13\n")
        assert connection.receive() == b"OK 13 PREVIEW_STOPPED\n"
        connection.close()
        wait_for(server.idle)
        for _ in range(8):
            connection = Connection()
            connection.send(b"PREVIEW_START 20\n")
            assert connection.receive() == b"OK 20 PREVIEW_STARTED\n"
            connection.send(b"PREVIEW_FRAME 21\nPREVIEW_FRAME 22\n")
            connection.close()  # 相机进行中断线，旧任务不得进入下一连接。
            wait_for(server.idle)
        connection = Connection()
        connection.send(b"x" * 128)
        assert connection.reader.read(1) == b""
        connection.close()
        server.stop()
        print("通过：分段/合并命令、编号校验、慢取图隔离、JPEG 连续发送、重复连接资源回收")
    finally:
        log = server.close()
        if sys.exc_info()[0] is not None:
            print(log)


def check_slow_reader_and_signal(executable):
    server = Server(executable, delay=0, size=4 * 1024 * 1024)
    connection = None
    try:
        connection = Connection(small_buffer=True)
        connection.send(b"CAPTURE_JPEG 1\n")
        assert connection.reader.readline(128) == b"JPEG 1 4194304\n"
        # 不读取载荷，等待发送期限触发，服务应回收连接和相机子进程。
        wait_for(server.idle, timeout=8)
        connection.close()
        connection = Connection(small_buffer=True)
        connection.send(b"CAPTURE_JPEG 2\n")
        assert connection.reader.readline(128) == b"JPEG 2 4194304\n"
        begin = time.monotonic()
        server.stop()
        assert time.monotonic() - begin < 4
        print("通过：慢客户端发送超时、背压中 SIGTERM 退出")
    finally:
        if connection is not None:
            connection.close()
        log = server.close()
        if sys.exc_info()[0] is not None:
            print(log)
    server = Server(executable)
    try:
        server.stop(signal.SIGINT)
        print("通过：空闲等待中的 SIGINT 退出")
    finally:
        server.close()


if __name__ == "__main__":
    assert sys.platform.startswith("linux"), "网络集成测试需要 Linux /proc"
    CAMERA_SERVICE = str(Path(sys.argv[2]).resolve())
    check_protocol_and_reconnect(str(Path(sys.argv[1]).resolve()))
    check_slow_reader_and_signal(str(Path(sys.argv[1]).resolve()))
