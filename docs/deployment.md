# 部署与日常运行

以下命令用于已核对板型、引脚、运行内核和设备树的开发环境。先完成 [构建](build.md) 和 [硬件接口](hardware.md) 检查。

## 板端文件

将用户态程序和匹配内核的模块传输到开发板，保留如下命名：

| 仓库产物 | 板端位置 |
| --- | --- |
| `server/tcp_ping_server` | `/home/root/tcp_ping_server_app` |
| `server/camera_service` | `/home/root/camera_service` |
| `drivers/icm20608/icm20608.ko` | `/home/root/icm20608-csfix1.ko` |
| `drivers/dht11/vehicle_dht11.ko` | `/home/root/vehicle_dht11.ko` |
| `drivers/sr04/vehicle_sr04.ko` | `/home/root/vehicle_sr04.ko` |
| `scripts/start-server.sh` | `/home/root/start-server.sh` |
| `scripts/vehicle-stack`（可选） | `/etc/init.d/vehicle-stack` |

脚本要求 ICM 版本 `stage03-csfix1`、DHT11 版本 `stage05-gpio2-v1`、SR04 版本 `stage05-gpio4-9-v1`。还依赖现有 AP3216C sysfs 驱动、GPIO1 最终 DTB、摄像头及 pinctrl debugfs。

开发板 Linux：

```sh
chmod +x /home/root/tcp_ping_server_app /home/root/camera_service
sh /home/root/start-server.sh --check /dev/video2
sh /home/root/start-server.sh /dev/video2
```

`--check` 只核对条件，不加载模块或修改 GPIO；模块未加载、GPIO 未导出时会报告缺失。普通启动会按脚本加载模块、导出 GPIO1 并设置为输入，服务在前台运行，Ctrl+C 结束。不要与已运行的 SysV 服务同时启动。

## 连接方式

**Wi-Fi TCP：** 电脑与开发板网络互通，确认开发板当前 IPv4、服务正在监听 9000，并按 [客户端构建说明](build.md) 更新目标地址。IP 可能在重启或 DHCP 续租后改变。

**USB/ADB：** 先沿用开发板已验证的 USB/ADB 配置。在 Windows PowerShell 执行：

```powershell
adb devices
adb -s vehicle-stage02 forward tcp:19000 tcp:9000
adb forward --list
```

如果序列号不同，替换为 `adb devices` 的实际结果。Qt 使用仓库默认 `127.0.0.1:19000`。USB/ADB 在两侧转发 TCP 数据，并非直接把开发板 IP 设为本地回环地址。

## 可选：SysV 服务入口

`scripts/vehicle-stack` 是现有板端 SysV 启动流程的整理副本，依赖 `/home/root/start-usb-adb.sh`。该 USB 初始化脚本与板端系统相关，未纳入仓库；如果没有此环境，先使用上面的前台入口。Wi-Fi 直连不需要 ADB 转发，但此历史 SysV 包装仍会调用 USB 初始化脚本。

将 `vehicle-stack` 安装到 `/etc/init.d/vehicle-stack` 并赋予执行权限后：

```sh
/etc/init.d/vehicle-stack start
/etc/init.d/vehicle-stack status
/etc/init.d/vehicle-stack stop
```

日志为 `/var/log/vehicle-stack.log`；PID 文件 `/var/run/vehicle-stack.pid` 有 PID 和 starttime 两个字段。重复 start 会识别已受管进程，不重复启动。开机入口需按当前系统的 SysV 运行级配置，仓库不自动安装或修改板端启动配置。

## 多进程版本升级与验收

先在 Ubuntu 用匹配 Linaro 工具链执行 `make -C server`，得到两个 ELF。分别核对 ARM hard-float ABI、加载器、动态库和符号版本，并记录 SHA-256。Windows 的 ARM 静态检查不代表交叉链接成功。

首次测试将两个程序和启动脚本放到独立目录，例如 `/home/root/mp-test-20261119/`，保持默认服务文件不变。用户先退出 Qt、停止旧服务，再执行：

```sh
chmod +x /home/root/mp-test-20261119/tcp_ping_server_mp /home/root/mp-test-20261119/camera_service
VEHICLE_SERVER_APP=/home/root/mp-test-20261119/tcp_ping_server_mp sh /home/root/mp-test-20261119/start-server.sh --check /dev/video2
VEHICLE_SERVER_APP=/home/root/mp-test-20261119/tcp_ping_server_mp sh /home/root/mp-test-20261119/start-server.sh /dev/video2
```

主进程不论连接与否均为 6 个线程；连接后应有一个单线程相机子进程，断线后子进程消失。相机由主进程管理，不再单独创建 SysV 启动入口。

```sh
pidof tcp_ping_server_mp
# 将下面的 PID 替换为实际主进程号
cat /proc/PID/status
cat /proc/PID/task/PID/children
ls -l /proc/PID/fd
```

验收包括五类采集、预览与拍照、场景联动、突发请求和完整错误回复。至少重复连接/取图/断线，核对两个进程的线程、FD 与总 RSS；进程版不能沿用旧版只测主进程的内存结论。

故障隔离测试必须先根据 `/proc/子PID/exe` 确认独立目录的 `camera_service`，再只终止该子进程；确认主 PID 不变、PING 与传感器仍能读取，并在有限重启后重新发送 PREVIEW_START 恢复图像。子进程连续失败超过两次重启后保持图像降级，重新建立客户端连接会开始新的恢复预算。

最后在预览/取帧中给主程序发送 SIGTERM，检查相机子进程被回收、端口释放和 Qt 断线；恢复原 `/etc/init.d/vehicle-stack start` 并复测 Qt。测试完成不自动替换默认程序。

正式升级时停止服务并备份旧文件，将两个程序作为同一版本放入默认位置，赋予执行权限，再启动并检查日志。回退恢复旧主程序和原启动入口；若旧版本已有配套相机程序，也恢复对应版本。用户态改造不需要重新刷 DTB 或模块。

DHT11 与 SR04 的时序锁只协调本服务；外部直接诊断读取前先停止服务。设备节点瞬时开关，检查 FD 时要辨别正常采样与残留对象。真实设备兼容、CPU、两进程总 RSS 和长期稳定性需实机测量。

## 排查顺序

先看服务日志及监听端口，再核对设备接口能否直接读取，最后检查连接地址、ADB 映射和 Qt 请求。`open` 成功不保证传感器采样成功；例如 DHT11 模块缺失会使接口不存在。相机异常需同时核对设备节点、V4L2 格式和占用情况。
