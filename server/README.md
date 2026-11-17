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

`ap3216c.c`、`icm20608.c`、`dht11.c`、`sr04.c`、`sr501.c` 是用户态接口适配层；`camera_capture.c` 负责 V4L2 缓冲与 JPEG 生命周期。设备读取错误记录在终端，并通过协议中的 `ERR` 返回。Qt 超时只结束客户端等待，不会取消板端正在执行的设备读取。
