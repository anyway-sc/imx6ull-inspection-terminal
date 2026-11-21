"""用本机 Clang 和已有 ARM 头文件检查服务源码，不生成板端可执行文件。"""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysroot", type=Path, required=True,
                        help="含 usr/include 的 Linaro ARM sysroot")
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--clang-tidy", type=Path,
                        default=Path("C:/Qt/Tools/QtCreator/bin/clang/bin/clang-tidy.exe"))
    args = parser.parse_args()
    args.sysroot = args.sysroot.resolve()
    args.work_dir = args.work_dir.resolve()
    if not (args.sysroot / "usr/include/linux/videodev2.h").is_file():
        parser.error("sysroot 缺少 ARM V4L2 头文件")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    server = Path(__file__).resolve().parents[1] / "server"
    sources = ("tcp_ping_server.c", "ap3216c.c", "icm20608.c", "dht11.c",
               "sr04.c", "sr501.c", "camera_capture.c", "sensor_sampler.c", "camera_worker.c",
               "camera_service.c", "ipc_protocol.c", "process_manager.c")
    for source in server.iterdir():
        if source.suffix in (".c", ".h"):
            shutil.copyfile(source, args.work_dir / source.name)
    for name in sources:
        command = [str(args.clang_tidy), str(args.work_dir / name),
                   "--checks=-*,clang-analyzer-core*,clang-analyzer-unix*",
                   "--warnings-as-errors=*", "--", "--target=arm-linux-gnueabihf",
                   f"--sysroot={args.sysroot}", "-std=gnu99", "-Wall", "-Wextra",
                   "-Wpedantic", "-Werror", "-pthread", "-ferror-limit=8"]
        result = subprocess.run(command, cwd=args.work_dir, text=True,
                                encoding="utf-8", errors="replace", capture_output=True)
        print(f"{name}：退出码 {result.returncode}", flush=True)
        for output in (result.stdout, result.stderr):
            if output.strip():
                print(output.strip(), flush=True)
        if result.returncode:
            return result.returncode
    print(f"{len(sources)} 个文件 ARM 目标语法和静态分析通过；未链接 ARM 程序，未访问硬件。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
