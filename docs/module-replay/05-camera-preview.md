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

验证结果将在实际执行后追加。
