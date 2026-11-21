# 板端启动脚本

`start-server.sh` 是前台入口：检查 9000 端口、相机、驱动版本、GPIO1 所属设备树及输入状态，准备模块和 GPIO 后运行服务。`--check` 仅检查条件。

`vehicle-stack` 是可选 SysV 包装：依次运行现有 USB/ADB 初始化、等待设备、启动前台入口，将日志和 PID/starttime 写入受管文件。支持 `start`、`stop`、`restart`、`status`。

默认路径和依赖见 [部署说明](../docs/deployment.md)。两份脚本都支持 `VEHICLE_*` 环境变量，用于路径适配和离线测试，实际参数以脚本顶部为准。本仓库不包含硬件相关的 `start-usb-adb.sh`，也不会自动修改板端 USB、无线或开机配置。

`build-on-ubuntu.sh` 在配套 ARM 工具链中成对构建主程序与相机程序，并输出 ABI / 动态库清单。`prepare-board-test.py --output /path/package.tar.gz` 生成可交叉编译的源码验收包，包含网络检查工具。操作顺序见 [多进程板端验收](../docs/process-board-test.md)。
