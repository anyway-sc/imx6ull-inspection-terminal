"""编译并运行真实线程管理代码，使用模拟设备，不访问开发板。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true",
                        help="在支持 ASan/UBSan 的 Linux 编译器上启用检查")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="vehicle-thread-check-") as temporary:
        work = Path(temporary)
        for source in (root / "server").glob("*.h"):
            shutil.copyfile(source, work / source.name)
        names = ("sensor_sampler.c", "fake_devices.c", "thread_check.c")
        for name in names:
            folder = "server" if name == "sensor_sampler.c" else "tests"
            shutil.copyfile(root / folder / name, work / name)
        shutil.copyfile(root / "tests/fake_devices.h", work / "fake_devices.h")
        executable = work / ("thread-check.exe" if sys.platform == "win32" else "thread-check")
        flags = ["-std=gnu99", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread", "-g", "-O1"]
        if sys.platform == "win32":
            flags += ["-DWINPTHREAD_STATIC", "-static"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command = [args.cc, *flags, *names, "-Wl,--wrap=pthread_create", "-o", str(executable)]
        environment = os.environ.copy()
        compiler = shutil.which(args.cc)
        if compiler:
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        for command in (command, [str(executable)]):
            result = subprocess.run(command, cwd=work, env=environment, capture_output=True, timeout=45,
                                    encoding="utf-8", errors="replace")
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                print(f"检查失败，退出码 {result.returncode}：{command[0]}", file=sys.stderr)
                return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
