# 相机预览与 Qt 交互：集成复盘

日期：2026-11-19。来源基线：`fd470565e244a84c97470a9e7b2b949e814e13cc`。

工作分支：`feat/replay-camera-preview`。本次将既有实现重新接入集成主线，原始来源声明保持原样。

## 本次接入

- `server/camera_capture.c`
- `server/camera_capture.h`
- `client/`
- `tests/dht11_check.cpp`
- `tests/imu_check.cpp`
- `tests/page_check.cpp`
- `tests/preview_check.cpp`
- `server/tcp_ping_server.c`
- `server/CMakeLists.txt`
- `server/Makefile`
- 按当前已接入模块更新服务分发和构建入口。



## 实际验证

被验证的代码提交：`9f2957e07fb176412100d0c0fbbce30b6e20d2da`。

- ARM 目标语法和 Clang core/unix 静态检查：5 个 C 文件通过，未交叉链接。
- 主机 CTest：5 项通过。
- Qt 6.8.3 / MinGW 13.1、Release 构建通过；Qt 使用模拟服务，不访问真实设备。

本次未执行内核模块构建、ARM 交叉链接或新增实机验证。Qt 界面在相机阶段统一接入已有集成实现；后续 SR04/SR501 阶段接入对应服务、驱动/设备树与专项回归。

```text
Internal ctest changing into directory: C:/Users/laiti/AppData/Local/Temp/vehicle-module-replay-q2obv0h8/src/build
Test project C:/Users/laiti/AppData/Local/Temp/vehicle-module-replay-q2obv0h8/src/build
    Start 5: Dht11FrameCheck
    Start 1: Dht11Check
1/5 Test #5: Dht11FrameCheck ..................   Passed    0.33 sec
    Start 2: ImuCheck
2/5 Test #2: ImuCheck .........................   Passed    8.17 sec
    Start 3: PageCheck
3/5 Test #1: Dht11Check .......................   Passed   10.20 sec
    Start 4: PreviewCheck
4/5 Test #3: PageCheck ........................   Passed    3.37 sec
5/5 Test #4: PreviewCheck .....................   Passed   10.58 sec

100% tests passed, 0 tests failed out of 5

Total Test time (real) =  20.94 sec
```
