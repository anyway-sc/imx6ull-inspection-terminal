# 板端 C 服务

`tcp_ping_server` 监听 IPv4 `0.0.0.0:9000`，支持五类传感器读取、单帧 JPEG 和按请求连续取帧。主循环为 `accept → handle_client → close`，一条连接结束后才处理下一条连接。

```sh
make -C server CC=arm-linux-gnueabihf-gcc
file server/tcp_ping_server server/camera_service
```

或在仓库根目录执行 `sh scripts/build-on-ubuntu.sh`，生成 `build-arm/tcp_ping_server_mp`、`build-arm/camera_service` 和 ABI / 依赖清单。当前管道版尚待匹配工具链正式链接及板端验收，不能沿用旧多线程版的板测结论。

两个程序需同次构建并成对部署。以下为按 [部署说明](../docs/deployment.md) 命名后的运行入口，首次验证应先使用 [独立测试目录](../docs/process-board-test.md)：

```sh
/home/root/tcp_ping_server_app /dev/video2
```

省略摄像头参数时默认使用 `/dev/video2`。该入口不会自动准备内核模块或 GPIO；日常使用见 [启动与部署](../docs/deployment.md) 和 [scripts](../scripts/README.md)。

`ap3216c.c`、`icm20608.c`、`dht11.c`、`sr04.c`、`sr501.c` 是用户态接口适配层；`camera_capture.c` 负责 V4L2 缓冲与 JPEG 生命周期。

新增 `sensor_sampler.c` 为五个设备各创建一个周期采集线程，用互斥锁保护完整快照、错误及采样时间；`READ_*` 返回最新有效快照，未就绪、最新读取失败或过期时返回原格式的 `ERR`。DHT11 每次读取结束后等待 3 秒，SR04/SR501 等待 500 ms，AP/ICM 等待 1 秒。

`camera_worker.c` 是主线程内的 IPC 代理，最多保留 4 项待执行任务、1 项在途任务和 1 项完整回复。`process_manager.c` 用两根非阻塞匿名管道连接由 posix_spawn 创建的 `camera_service`：FD 3 读任务，FD 4 写结果。相机子进程独占 V4L2；主线程完整接收 JPEG 后统一发送，慢客户端超过单条回复 5 秒期限后断开。两端处理 EOF/EPIPE，忽略 SIGPIPE；相机故障触发有限重启，传感器与 TCP 继续服务。

主服务和相机服务必须成对部署，相机程序默认位于主程序旁。SIGINT/SIGTERM 唤醒主循环，关闭两根管道的父进程端点，终止并回收相机子进程，停止并 join 采集线程，释放 JPEG、同步对象与描述符。已进入的驱动调用需要等其返回，不通过 `pthread_cancel` 强制取消。Qt 的超时只结束上位机等待。资源归属见 [架构说明](../docs/architecture.md)。
