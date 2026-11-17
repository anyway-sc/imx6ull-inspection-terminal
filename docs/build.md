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
file server/tcp_ping_server
```

交叉工具链的 CPU、浮点 ABI、动态加载器和 libc 必须与板端兼容。服务已在配套 Ubuntu ARM 工具链中构建并部署；本次整理的本机 ARM 语法检查不能替代完整链接。

在 Linux 本机也可仅编译服务用于开发检查：

```sh
cmake -S . -B build-server -DVEHICLE_BUILD_CLIENT=OFF -DBUILD_TESTING=OFF
cmake --build build-server
```

主机上的服务缺少开发板设备时，对相应命令返回读取失败是预期行为。

## 外置内核模块

需要与运行内核匹配、已配置及编译准备好的 Linux 4.1.15 BSP，包含匹配的 `.config`、生成头文件和 `Module.symvers`。不能使用 Ubuntu 主机内核头文件构建后直接加载到开发板。

```sh
make -C drivers/icm20608 KERNELDIR=/path/to/matched-linux ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
make -C drivers/dht11 KERNELDIR=/path/to/matched-linux ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
make -C drivers/sr04 KERNELDIR=/path/to/matched-linux ARCH=arm CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
```

`KERNELDIR` 和 `CROSS_COMPILE` 都需要按实际 BSP 设置。模块 Makefile 中的默认路径为原开发环境，不作为其他电脑的通用路径。生成 `icm20608.ko`、`vehicle_dht11.ko` 和 `vehicle_sr04.ko`；部署前核对 `modinfo`、vermagic 和内核符号版本。
