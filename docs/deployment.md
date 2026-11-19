# 部署与日常运行

以下命令用于已核对板型、引脚、运行内核和设备树的开发环境。先完成 [构建](build.md) 和 [硬件接口](hardware.md) 检查。

## 板端文件

将用户态程序和匹配内核的模块传输到开发板，保留如下命名：

| 仓库产物 | 板端位置 |
| --- | --- |
| `server/tcp_ping_server` | `/home/root/tcp_ping_server_app` |
| `drivers/icm20608/icm20608.ko` | `/home/root/icm20608-csfix1.ko` |
| `drivers/dht11/vehicle_dht11.ko` | `/home/root/vehicle_dht11.ko` |
| `drivers/sr04/vehicle_sr04.ko` | `/home/root/vehicle_sr04.ko` |
| `scripts/start-server.sh` | `/home/root/start-server.sh` |
| `scripts/vehicle-stack`（可选） | `/etc/init.d/vehicle-stack` |

脚本要求 ICM 版本 `stage03-csfix1`、DHT11 版本 `stage05-gpio2-v1`、SR04 版本 `stage05-gpio4-9-v1`。还依赖现有 AP3216C sysfs 驱动、GPIO1 最终 DTB、摄像头及 pinctrl debugfs。

开发板 Linux：

```sh
chmod +x /home/root/tcp_ping_server_app
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

## 多线程版本升级与验收

先在 Ubuntu 使用匹配工具链重新编译九文件服务；Makefile 已包含 `-pthread`。板端先停止旧服务并确认进程退出，保存旧程序备份，再替换 `/home/root/tcp_ping_server_app` 并恢复执行权限。线程改造发生在用户态，当前驱动和 DTB 沿用既有配置；不要把旧程序 BuildID 的实机结果当作新版本验收。

首次运行时确认日志显示多线程服务。正常空闲时主线程加五个采集线程共 6 个，Qt 连接后增加一个相机线程共 7 个。使用 SysV 启动时可检查：

```sh
read vehicle_pid vehicle_starttime < /var/run/vehicle-stack.pid
ls "/proc/$vehicle_pid/task"
ls "/proc/$vehicle_pid/fd"
```

应验证传感器数值变化、相机启停/拍照、两类场景联动、连续断线重连后的线程与 FD 回收，以及 SIGTERM 后退出和重新启动。采样已改为后台定时更新，重复请求可能得到同一份有效样本；启动最初尚未采样完成时可能返回读取失败。记录新可执行文件摘要、线程/FD 数、RSS 和业务结果，性能数字需要重新实测。

DHT11 与 SR04 的共用时序锁只覆盖本服务内部。需要单独直接读取这两个设备做诊断时，先停止后台服务，避免另一个进程绕过应用层时序协调。

## 排查顺序

先看服务日志及监听端口，再核对设备接口能否直接读取，最后检查连接地址、ADB 映射和 Qt 请求。`open` 成功不保证传感器采样成功；例如 DHT11 模块缺失会使接口不存在。相机异常需同时核对设备节点、V4L2 格式和占用情况。
