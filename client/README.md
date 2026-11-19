# Qt 客户端

本目录取自实际 `VehiclePanel` 工程，保留五个业务页面及现有交互。整理副本把现场 Wi-Fi 地址恢复为 `127.0.0.1:19000`，实际运行工程保持原样。

需要 Qt 6.5 以上的 Core、Widgets、Network、LinguistTools；已验证工具链为 Qt 6.8.3 / MinGW GCC 13.1.0 / CMake。既可在 Qt Creator 中打开本目录的 `CMakeLists.txt`，也可从仓库根目录同时编译客户端与测试，见 [构建说明](../docs/build.md)。

网络入口为 `mainwindow.cpp` 的 `MainWindow::connectToServer()`。Wi-Fi 直连时将 `connectToHost()` 改为开发板实际 IPv4 和 `9000`，同时更新构造函数中的目标地址标签。当前没有地址配置界面或自动重连。
