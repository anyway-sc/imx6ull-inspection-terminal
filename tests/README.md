# 本机检查

从仓库根目录按 [构建说明](../docs/build.md) 配置、编译，再运行 `ctest --test-dir build --output-on-failure --parallel 2`。

| CTest 目标 | 范围 |
| --- | --- |
| SentinelCheck | SR501 信号、未知状态、连续低电平计时及相机交接 |
| Sr04Check | 距离回复、重试与倒车页行为 |
| Dht11Check | 温湿度解析、请求匹配和自动刷新 |
| ImuCheck | ICM 原始数据、单位转换和边界 |
| PageCheck | 页面切换及相机状态交接 |
| PreviewCheck | JPEG 拆分接收、预览启停和错误行为 |
| sr501ReadCheck / sr04ReadCheck | 临时文件文本解析、边界与文件错误 |
| Dht11FrameCheck | 驱动共享的 DHT11 帧校验纯逻辑 |
| Sr04CaptureCheck | 驱动共享的回波状态与换算纯逻辑 |

Qt 使用 `QT_QPA_PLATFORM=offscreen`，测试不会连接真实开发板。生成的 `*-mock.png` 是模拟画面，不是实机截图。

启动脚本检查需要 Python 3.10 以上、Git for Windows 的 Bash/awk 等工具；本机已用 Git 安装在 `D:/Git` 的环境验证：

```powershell
python tests/startup_check.py
python tests/sysv_startup_check.py
```

这两项使用临时 sysfs、测试命令及受管测试进程。SysV 检查包含专门针对 MSYS `/proc` 行为的适配，只在 Windows Git/MSYS 环境运行；不把它当作 Linux 实机验证。

可选的七文件 ARM 静态检查：

```powershell
python tests/check_server_arm.py --sysroot C:/path/to/arm-sysroot --work-dir arm-check --clang-tidy C:/path/to/clang-tidy.exe
```

sysroot 需包含目标平台的 `usr/include/linux/videodev2.h`。此步骤检查 ARM 语法和 Clang core/unix 分析规则，不执行 GCC 交叉链接。
