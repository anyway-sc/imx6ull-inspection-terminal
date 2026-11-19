# SR501 活动触发画面：集成复盘

日期：2026-11-19。来源基线：`fd470565e244a84c97470a9e7b2b949e814e13cc`。

工作分支：`feat/replay-sr501-sentinel`。本次将既有实现重新接入集成主线，原始来源声明保持原样。

## 本次接入

- `server/sr501.c`
- `server/sr501.h`
- `device-tree/`
- `tests/sr501_read_check.c`
- `tests/sentinel_check.cpp`
- `server/tcp_ping_server.c`
- `server/CMakeLists.txt`
- `server/Makefile`
- 按当前已接入模块更新服务分发和构建入口。



## 实际验证

被验证的代码提交：`614e8c3634f2bd2776c31a9695ce69a57aac650b`。

- ARM 目标语法和 Clang core/unix 静态检查：7 个 C 文件通过，未交叉链接。
- 主机 CTest：10 项通过。
- Qt 6.8.3 / MinGW 13.1、Release 构建通过；Qt 使用模拟服务，不访问真实设备。

本次未执行内核模块构建、ARM 交叉链接或新增实机验证。Qt 界面在相机阶段统一接入已有集成实现；后续 SR04/SR501 阶段接入对应服务、驱动/设备树与专项回归。

```text
Internal ctest changing into directory: C:/Users/laiti/AppData/Local/Temp/vehicle-module-replay-q2obv0h8/src/build
Test project C:/Users/laiti/AppData/Local/Temp/vehicle-module-replay-q2obv0h8/src/build
      Start  6: PreviewCheck
      Start  3: Dht11Check
 1/10 Test  #3: Dht11Check .......................   Passed   10.17 sec
      Start  4: ImuCheck
 2/10 Test  #6: PreviewCheck .....................   Passed   10.58 sec
      Start  2: Sr04Check
 3/10 Test  #2: Sr04Check ........................   Passed    7.06 sec
      Start  5: PageCheck
 4/10 Test  #4: ImuCheck .........................   Passed    8.13 sec
      Start  8: sr04ReadCheck
 5/10 Test  #8: sr04ReadCheck ....................   Passed    0.15 sec
      Start  9: Dht11FrameCheck
 6/10 Test  #9: Dht11FrameCheck ..................   Passed    0.12 sec
      Start 10: Sr04CaptureCheck
 7/10 Test #10: Sr04CaptureCheck .................   Passed    0.11 sec
      Start  1: SentinelCheck
 8/10 Test  #5: PageCheck ........................   Passed    3.31 sec
      Start  7: sr501ReadCheck
 9/10 Test  #7: sr501ReadCheck ...................   Passed    0.17 sec
10/10 Test  #1: SentinelCheck ....................   Passed   15.54 sec

100% tests passed, 0 tests failed out of 10

Total Test time (real) =  34.38 sec
```
