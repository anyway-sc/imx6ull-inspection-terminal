# SR04 距离与画面联合监测：集成复盘

日期：2026-11-19。来源基线：`fd470565e244a84c97470a9e7b2b949e814e13cc`。

工作分支：`feat/replay-sr04-monitor`。本次将既有实现重新接入集成主线，原始来源声明保持原样。

## 本次接入

- `server/sr04.c`
- `server/sr04.h`
- `drivers/sr04/`
- `tests/sr04_read_check.c`
- `tests/sr04_check.cpp`
- `tests/capture_check.c`
- `server/tcp_ping_server.c`
- `server/CMakeLists.txt`
- `server/Makefile`
- 按当前已接入模块更新服务分发和构建入口。



## 实际验证

被验证的代码提交：`0da417f255f50698343f9e0a48bb491667e1f9cd`。

- ARM 目标语法和 Clang core/unix 静态检查：6 个 C 文件通过，未交叉链接。
- 主机 CTest：8 项通过。
- Qt 6.8.3 / MinGW 13.1、Release 构建通过；Qt 使用模拟服务，不访问真实设备。

本次未执行内核模块构建、ARM 交叉链接或新增实机验证。Qt 界面在相机阶段统一接入已有集成实现；后续 SR04/SR501 阶段接入对应服务、驱动/设备树与专项回归。

```text
Internal ctest changing into directory: C:/Users/laiti/AppData/Local/Temp/vehicle-module-replay-q2obv0h8/src/build
Test project C:/Users/laiti/AppData/Local/Temp/vehicle-module-replay-q2obv0h8/src/build
    Start 5: PreviewCheck
    Start 2: Dht11Check
1/8 Test #2: Dht11Check .......................   Passed   10.18 sec
    Start 3: ImuCheck
2/8 Test #5: PreviewCheck .....................   Passed   10.53 sec
    Start 4: PageCheck
3/8 Test #4: PageCheck ........................   Passed    3.33 sec
    Start 7: Dht11FrameCheck
4/8 Test #7: Dht11FrameCheck ..................   Passed    0.17 sec
    Start 1: Sr04Check
5/8 Test #3: ImuCheck .........................   Passed    8.14 sec
    Start 6: sr04ReadCheck
6/8 Test #6: sr04ReadCheck ....................   Passed    0.24 sec
    Start 8: Sr04CaptureCheck
7/8 Test #8: Sr04CaptureCheck .................   Passed    0.16 sec
8/8 Test #1: Sr04Check ........................   Passed    7.13 sec

100% tests passed, 0 tests failed out of 8

Total Test time (real) =  21.17 sec
```
