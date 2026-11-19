"""在电脑临时目录模拟 sysfs 与命令，检查启动脚本；不访问开发板。"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

script = Path(__file__).resolve().parents[1] / "scripts/start-server.sh"
shell = Path(os.environ.get("SR501_TEST_SHELL", "D:/Git/bin/bash.exe"))
checks = 0

def check(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)

with tempfile.TemporaryDirectory(prefix="startup-fixture-", dir=Path.cwd()) as directory:
    base = Path(directory)
    sysroot = base / "sys"
    commands = base / "bin"
    commands.mkdir()

    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    write(commands / "netstat", '#!/bin/sh\nprintf "Proto Recv-Q Send-Q Local_Address Foreign_Address State PID\\n"\n')
    write(commands / "insmod", '''#!/bin/sh
case "$1" in
  *vehicle_dht11.ko) name=vehicle_dht11; version=stage05-gpio2-v1 ;;
  *vehicle_sr04.ko) name=vehicle_sr04; version=stage05-gpio4-9-v1 ;;
  *) exit 1 ;;
esac
mkdir -p "$VEHICLE_SYS_ROOT/module/$name" "$VEHICLE_SYS_ROOT/class/misc/$name"
printf '%s\\n' "$version" > "$VEHICLE_SYS_ROOT/module/$name/version"
printf '0\\n' > "$VEHICLE_SYS_ROOT/class/misc/$name/value"
''')
    write(base / "server", '#!/bin/sh\nprintf "MOCK_SERVER_EXECUTED %s\\n" "$1"\n')
    for name, version in (("icm20608", "stage03-csfix1"), ("vehicle_dht11", "stage05-gpio2-v1"), ("vehicle_sr04", "stage05-gpio4-9-v1")):
        write(sysroot / "module" / name / "version", version + "\n")
        write(sysroot / "class/misc" / name / "value", "0\n")
    for node in ("dht11", "ds18b20"):
        write(sysroot / "firmware/devicetree/base" / node / "status", "disabled\0")
    write(sysroot / "kernel/debug/pinctrl/20e0000.iomuxc/pinmux-pins", "pin 24 (MX6UL_PAD_GPIO1_IO01): 20e0000.iomuxc (HOG) function imx6ul-evk group hoggrp-1\n")
    write(sysroot / "class/gpio/gpiochip0/base", "0\n")
    write(sysroot / "class/gpio/gpiochip0/label", "209c000.gpio\n")
    gpio = sysroot / "class/gpio/gpio1"
    write(gpio / "direction", "in\n")
    write(gpio / "active_low", "0\n")
    write(gpio / "value", "0\n")
    write(sysroot / "class/gpio/export", "")
    write(base / "vehicle_dht11.ko", "fixture\n")
    write(base / "vehicle_sr04.ko", "fixture\n")
    env = os.environ.copy()
    commands_path = commands.as_posix()
    if commands.drive:
        commands_path = "/" + commands.drive[0].lower() + commands_path[2:]
    env.update({
        "PATH": commands_path + ":/d/Git/usr/bin:/d/Git/bin",
        "VEHICLE_SYS_ROOT": sysroot.as_posix(),
        "VEHICLE_SERVER_APP": (base / "server").as_posix(),
        "VEHICLE_ICM_SYS_DIR": (sysroot / "module/icm20608").as_posix(),
        "VEHICLE_ICM_DEVICE": "/dev/null",
        "VEHICLE_DHT_MODULE": (base / "vehicle_dht11.ko").as_posix(),
        "VEHICLE_SR04_MODULE": (base / "vehicle_sr04.ko").as_posix(),
    })

    def run(*args):
        result = subprocess.run([str(shell), str(script), *args, "/dev/null"], env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15)
        return result.returncode, result.stdout + result.stderr

    def snapshot():
        return {p.relative_to(base).as_posix(): p.read_bytes() for p in base.rglob("*") if p.is_file()}

    before = snapshot()
    code, output = run("--check")
    check(code == 0, output)
    check(snapshot() == before, "--check 不应写入文件")
    check("MOCK_SERVER_EXECUTED" not in output, "--check 不应执行服务")
    write(gpio / "direction", "out\n")
    before = snapshot()
    code, output = run("--check")
    check(code != 0 and snapshot() == before, "--check 对错误方向只报错，不修改")
    write(gpio / "active_low", "1\n")
    code, output = run()
    check(code == 0 and "MOCK_SERVER_EXECUTED /dev/null" in output, output)
    check((gpio / "direction").read_text() == "in\n" and (gpio / "active_low").read_text() == "0\n", "正常启动恢复输入和极性")
    write(sysroot / "firmware/devicetree/base/dht11/status", "okay\0")
    write(gpio / "direction", "out\n")
    code, output = run()
    check(code != 0 and "MOCK_SERVER_EXECUTED" not in output, "旧DTB应拒绝启动")
    check((gpio / "direction").read_text() == "out\n", "旧DTB下不写GPIO方向")
    write(sysroot / "firmware/devicetree/base/dht11/status", "disabled\0")
    for name in ("vehicle_dht11", "vehicle_sr04"):
        shutil.rmtree(sysroot / "module" / name)
    code, output = run()
    check(code == 0 and "MOCK_SERVER_EXECUTED" in output, output)
    check((sysroot / "module/vehicle_dht11/version").exists() and (sysroot / "module/vehicle_sr04/version").exists(), "正常启动加载缺失的已有模块")
    write(sysroot / "module/vehicle_sr04/version", "wrong\n")
    code, output = run()
    check(code != 0 and "MOCK_SERVER_EXECUTED" not in output, "错误模块版本拒绝启动")
    write(commands / "netstat", '#!/bin/sh\nprintf "tcp 0 0 0.0.0.0:9000 0.0.0.0:* LISTEN 99999/mock\\n"\n')
    before = snapshot()
    code, output = run()
    check(code != 0 and snapshot() == before, "端口忙时不修改设备")
print(f"通过：{checks}项启动脚本模拟检查；未运行真实服务或访问硬件。")
