# 多进程版本板端验收

本包包含十二个 C 文件、头文件、双程序 Makefile、Ubuntu 构建脚本和网络检查工具。它是源码包，不含新 ARM 可执行文件，不自动替换板端默认服务。此前多线程版的板测记录不能作为本版验收结果。

## Ubuntu 编译

将压缩包上传到 `/home/book/vehicle-learning`，在 Ubuntu 执行：

```sh
cd /home/book/vehicle-learning
tar -xzf multiprocess-board-test-20261119.tar.gz
cd multiprocess-board-test-20261119
sh build-on-ubuntu.sh
```

输出为 `build-arm/tcp_ping_server_mp`、`build-arm/camera_service`、`build-arm/build-manifest.txt`。将三个文件下载到 Windows。分别核对两个 ELF 的 hard-float ABI、加载器、NEEDED 和 GLIBC 要求；开发板是 GNU libc 2.23，需使用原配 Linaro 工具链。不要将 CI 的 x86 Linux 程序上传到 ARM 板。

## 独立目录试运行

用户先退出 Qt，保留旧程序，停止 `/etc/init.d/vehicle-stack`。使用已验证 ADB 序列号 `vehicle-stage02` 上传两个可执行文件和本包 `start-server.sh` 至 `/home/root/mp-test-20261119/`。两程序都需执行权限。

在板端通过 ADB shell 执行：

```sh
chmod 755 /home/root/mp-test-20261119/tcp_ping_server_mp /home/root/mp-test-20261119/camera_service
sha256sum /home/root/mp-test-20261119/tcp_ping_server_mp /home/root/mp-test-20261119/camera_service
/lib/ld-linux-armhf.so.3 --list /home/root/mp-test-20261119/tcp_ping_server_mp
/lib/ld-linux-armhf.so.3 --list /home/root/mp-test-20261119/camera_service
VEHICLE_SERVER_APP=/home/root/mp-test-20261119/tcp_ping_server_mp sh /home/root/mp-test-20261119/start-server.sh --check /dev/video2
VEHICLE_SERVER_APP=/home/root/mp-test-20261119/tcp_ping_server_mp sh /home/root/mp-test-20261119/start-server.sh /dev/video2
```

保持前台窗口运行，在另一个 Windows PowerShell 执行：

```powershell
& 'D:\tool\platform-tools-latest-windows\platform-tools\adb.exe' -s vehicle-stage02 forward --no-rebind tcp:19001 tcp:9000
python board_tcp_check.py --host 127.0.0.1 --port 19001 --mode smoke
python board_tcp_check.py --host 127.0.0.1 --port 19001 --mode reconnect --cycles 10
python board_tcp_check.py --host 127.0.0.1 --port 19001 --mode queue
```

网络工具需要 Python 和 Pillow；也可沿用此前已验证的 Codex Python 路径。检查工具与 Qt 不要同时连接。先在超声波前放置平整物体，再检查测距，设备错误需结合物理接线判断。

## 资源及故障验收

用 `--mode hold` 保持网络连接，在另一个窗口读取 `/proc/主PID/task/主PID/children`。主进程始终预期 6 个线程，连接时存在一个单线程相机子进程；断线后子进程消失。分别记录两个进程的状态、FD、RSS，不能只记录父进程内存。

故障测试先核对 `/proc/子PID/exe` 指向本目录 `camera_service`，再只终止相机子进程。确认主 PID 不变，同一 TCP 连接的 PING / 传感器仍有回复；恢复后重新发送 PREVIEW_START。每连接最多初次启动加两次重启，不会永久自动重启。

原来的 Qt 程序会在预览错误时断开连接。验证图像故障后继续使用界面，需要使用仓库新 Qt 副本构建的程序；新客户端对已释放流的相机错误保留 TCP，损坏协议和半帧超时仍断线。SR501 可继续监测；距离联合观察停止图像后可手动测距。

最后在预览或取帧时向已确认的主 PID 发送 SIGTERM，确认父子进程退出、9000 端口释放。恢复 `/etc/init.d/vehicle-stack start`，检查状态并复测 Qt，移除临时 ADB 转发。不要在未完成本次验收前替换默认程序。

将两个程序 SHA-256、编译清单、资源快照、网络结果 JSON 和 Qt 人工反馈一起归档。实机验收范围是功能、有限重连、错误路径和回收；精度、长期稳定性及端到端性能另行测量。
