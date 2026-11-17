# TCP 协议

服务监听 IPv4 TCP 9000。所有命令区分大小写，字段间为单个 ASCII 空格，文本消息以 LF (`0x0A`) 结束，不发送 C 字符串末尾 NUL。命令最多 127 字节，不计 LF；超长时关闭当前连接。

`id` 为十进制整数 `1..4294967295`，不接受符号、空格或溢出。Qt 共用递增请求编号，只接受当前请求匹配的业务结果。TCP 允许任意拆分或合并消息，接收端按缓存与协议边界解释。

| 请求 | 成功回复 |
| --- | --- |
| `PING` | `PONG` |
| `READ_AP3216C <id>` | `AP3216C <id> <ir> <als> <ps>` |
| `READ_ICM20608 <id>` | `ICM20608 <id> <gx> <gy> <gz> <ax> <ay> <az> <temp>` |
| `READ_DHT11 <id>` | `DHT11 <id> <humidity_tenths> <temperature_tenths>` |
| `READ_SR04 <id>` | `SR04 <id> <distance_mm> <pulse_us>` |
| `READ_SR501 <id>` | `SR501 <id> <level>`，level 为 0 或 1 |
| `CAPTURE_JPEG <id>` | `JPEG <id> <length>` + LF，紧接 length 字节 JPEG |
| `PREVIEW_START <id>` | `OK <id> PREVIEW_STARTED` |
| `PREVIEW_FRAME <id>` | 同 JPEG 长度头及载荷 |
| `PREVIEW_STOP <id>` | `OK <id> PREVIEW_STOPPED` |

DHT11 温湿度单位为十分之一，例如 `DHT11 7 600 260\n` 表示 60.0%RH、26.0°C。ICM 字段为驱动原始有符号整数；AP 字段为非负原始计数。

## JPEG 分帧

例如头部 `JPEG 9 12000\n` 后，立刻读取 12000 字节 JPEG。本体中可能包含 LF 和 NUL，没有额外分隔符。必须消费完本体再解析下一条头，即使编号已过期。单张 JPEG 上限为 4 MiB。

START/STOP 可重复调用；FRAME 要求流已开启。流开启时 CAPTURE 返回 `CAMERA_BUSY`。服务在 STOP、相机错误和连接结束时关闭流。

## 错误

采样失败返回 `ERR <id> AP3216C_READ`、`ICM20608_READ`、`DHT11_READ`、`SR04_READ` 或 `SR501_READ`。相机错误包括 `CAMERA_CAPTURE`、`CAMERA_START`、`CAMERA_FRAME`、`CAMERA_STOP`、`PREVIEW_NOT_STARTED` 和 `CAMERA_BUSY`。非法编号返回 `ERR BAD_REQUEST`；未识别命令返回 `ERR UNKNOWN_COMMAND`。

## Qt 定时与业务策略

| 业务 | 策略 |
| --- | --- |
| AP / ICM 自动刷新 | 每 1 秒尝试一次，等待中跳过 |
| DHT11 自动刷新 | 每 3 秒尝试一次 |
| SR501 监测 | 每 500ms 尝试采样；超过 2 秒未更新标为未知 |
| 哨兵停止画面 | 连续有效低电平至少 10 秒；错误、过期和未知会清除计时 |
| 采样请求超时 | 3 秒 |
| 相机请求及载荷超时 | 8 秒 |
| 连续预览 | 显示后约 100ms 再请求一帧，不代表实测 10 FPS |

SR04 采样失败继续重试并保留倒车画面，SR501 采样失败不当作低电平。协议错误、超时和相机失败按客户端状态停止业务或关闭连接。客户端超时不会取消板端正在执行的设备读取。
