# 构建

## Windows Qt 客户端及测试

已验证环境：Qt 6.8.3，MinGW GCC 13.1.0，CMake，Ninja。Qt 需安装 Core、Widgets、Network、LinguistTools。建议将仓库克隆至纯英文目录，例如 `D:/Projects/imx6ull-vehicle-terminal`，避免本机 MinGW/Qt 工具对中文路径的兼容问题。

在仓库根目录打开 PowerShell，按本机安装位置调整以下路径：

```powershell
$env:PATH = "C:\Qt\6.8.3\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;" + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/mingw_64 -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release -DVEHICLE_BUILD_SERVER=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --parallel 2
```

使用上述单配置构建时，客户端为 `build/VehiclePanel.exe`。只编译应用时添加 `-DBUILD_TESTING=OFF`。Qt Creator 可直接打开根目录或 `client/CMakeLists.txt`。

默认目标为 `127.0.0.1:19000`，用于 ADB 转发。Wi-Fi 直连时在 `client/mainwindow.cpp` 中同时修改：

1. 构造函数的 `targetLabel` 地址显示。
2. `MainWindow::connectToServer()` 中的主机地址和端口，例如 `socket->connectToHost("192.168.1.100", 9000);`，示例 IP 应替换为当前开发板地址。

## Linux / ARM 用户态服务

```sh
make -C server CC=arm-linux-gnueabihf-gcc
file server/tcp_ping_server server/camera_service
```

交叉工具链的 CPU、浮点 ABI、动态加载器和 libc 必须与板端兼容。此前多线程版已在配套 Ubuntu ARM 工具链中构建并试运行；当前双管道进程版尚未完成正式 ARM 链接及板端验收，ARM 语法检查不能替代完整链接。

在仓库根目录也可使用配套构建脚本：

```sh
sh scripts/build-on-ubuntu.sh
```

脚本默认使用 PATH 中的 `arm-linux-gnueabihf-gcc`，可将编译器完整路径作为第一个参数传入。输出为 `build-arm/tcp_ping_server_mp`、`build-arm/camera_service` 和 `build-arm/build-manifest.txt`；清单记录两个 ELF 的摘要、ABI、加载器和运行库要求。后续步骤见 [管道版板端验收](process-board-test.md)。

在 Linux 本机也可仅编译服务用于开发检查：

```sh
cmake -S . -B build-server -DVEHICLE_BUILD_CLIENT=OFF -DBUILD_TESTING=OFF
cmake --build build-server
```

主机上的服务缺少开发板设备时，对相应命令返回读取失败是预期行为。

构建会生成 `tcp_ping_server` 和 `camera_service` 两个程序，部署时必须成对传输，并将相机程序放在主程序旁，或以 `VEHICLE_CAMERA_SERVICE` 指定其绝对路径。主程序使用 `-pthread`，相机程序独占 V4L2。Makefile 支持 `TARGET=/path/tcp_ping_server_mp`，配套相机程序自动输出为 `/path/camera_service`。Linux 主机也可在不安装 Qt 的情况下构建服务及模拟测试：

```sh
cmake -S . -B build-server -DVEHICLE_BUILD_CLIENT=OFF -DBUILD_TESTING=ON
cmake --build build-server
ctest --test-dir build-server --output-on-failure
```

此时 Qt 测试默认关闭，运行四项纯 C 检查、采集线程检查、Linux 网络模拟检查、多进程故障检查和管道专项检查，共 8 项；均不访问板端设备。网络模拟检查要求本机 9000 端口空闲。

## 外置内核模块

需要与运行内核匹配、已配置及编译准备好的 Linux 4.1.15 BSP，包含匹配的 `.config`、生成头文件和 `Module.symvers`。不能使用 Ubuntu 主机内核头文件构建后直接加载到开发板。

```sh
make -C drivers/icm20608 KERNELDIR=/path/to/matched-linux ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
make -C drivers/dht11 KERNELDIR=/path/to/matched-linux ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
make -C drivers/sr04 KERNELDIR=/path/to/matched-linux ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
```

`KERNELDIR` 和 `CROSS_COMPILE` 都需要按实际 BSP 设置。模块 Makefile 中的默认路径为原开发环境，不作为其他电脑的通用路径。生成 `icm20608.ko`、`vehicle_dht11.ko` 和 `vehicle_sr04.ko`；部署前核对 `modinfo`、vermagic 和内核符号版本。
