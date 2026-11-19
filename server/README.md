# 板端 C 服务

`tcp_ping_server` 监听 IPv4 `0.0.0.0:9000`，支持五类传感器读取、单帧 JPEG 和按请求连续取帧。主循环为 `accept → handle_client → close`，一条连接结束后才处理下一条连接。

```sh
make -C server CC=arm-linux-gnueabihf-gcc
file server/tcp_ping_server
```

在开发板上直接运行：

```sh
/home/root/tcp_ping_server_app /dev/video2
```

省略摄像头参数时默认使用 `/dev/video2`。该入口不会自动准备内核模块或 GPIO；日常使用见 [启动与部署](../docs/deployment.md) 和 [scripts](../scripts/README.md)。

`ap3216c.c`、`icm20608.c`、`dht11.c`、`sr04.c`、`sr501.c` 是用户态接口适配层；`camera_capture.c` 负责 V4L2 缓冲与 JPEG 生命周期。

新增 `sensor_sampler.c` 为五个设备各创建一个周期采集线程，用互斥锁保护完整快照、错误及采样时间；`READ_*` 返回最新有效快照，未就绪、最新读取失败或过期时返回原格式的 `ERR`。DHT11 每次读取结束后等待 3 秒，SR04/SR501 等待 500 ms，AP/ICM 等待 1 秒。

新增 `camera_worker.c` 独占相机，通过 4 项请求队列和 2 项回复队列与主线程交互。主线程独占网络发送，JPEG 头和载荷连续发送，慢客户端超过单条回复 5 秒期限后断开。

SIGINT/SIGTERM 唤醒主循环，停止线程、join 并回收相机、JPEG、同步对象与描述符。已进入的驱动调用需要等其返回，不通过 `pthread_cancel` 强制取消。Qt 的超时只结束上位机等待。资源归属见 [架构说明](../docs/architecture.md)。
