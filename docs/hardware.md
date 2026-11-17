# 硬件、接线与设备接口

实测硬件为正点原子 ALPHA V2.8 / i.MX6ULL、ARMv7 小端、512 MiB 内存、eMMC，运行原 Linux 4.1.15。其他底板版本需重新核对原理图与引脚复用。

| 模块 | 接线 / 总线 | 用户态接口 |
| --- | --- | --- |
| AP3216C | 板载 I²C，依赖已适配的板载驱动 | `/sys/class/misc/ap3216c/{ir,als,ps}` |
| ICM20608 | 板载 SPI，旧片选属性兼容 | `/dev/icm20608`，7 个有符号 32 位原始值，共 28 字节 |
| DHT11 | DATA→GPIO1_IO02 / JP6-19；VCC→VOUT1-2 3.3V；GND→VOUT1-1 | `/sys/class/misc/vehicle_dht11/value`，`湿度,温度` |
| HC-SR04-RC | Trig→GPIO1_IO04 / JP6-20；Echo→GPIO1_IO09 / JP6-16；VCC→VOUT1-4 3.3V；GND→VOUT1-3 | `/sys/class/misc/vehicle_sr04/value`，`距离mm,回波μs` |
| HC-SR501 | OUT→GPIO1_IO01 / JP6-17；VCC→VOUT2-2 5V；GND→VOUT2-1 | `/sys/class/gpio/gpio1/{direction,active_low,value}` |
| USB 摄像头 | 已验证的 USB_HOST1 接法，MJPEG | `/dev/video2`，可用服务参数更改 |

SR04 使用已确认支持 3.3V 供电的 HC-SR04-RC 版本，Echo 必须兼容 3.3V GPIO；不能直接将其他型号的 5V Echo 接入。HC-SR501 的 5V 供电与 OUT 约 3.3V 是不同信号。模块针序按实物标识核对。

AP3216C 适配驱动是板端环境依赖，本仓库没有其对应内核实现；原厂例程的 `/dev/ap3216c` 二进制协议不能直接替代以上三个 sysfs 属性。Linux BSP、完整 DTB 和板端 USB/ADB 初始化脚本也需沿用已验证环境。

ICM20608 的字段顺序为 `gx, gy, gz, ax, ay, az, temp`。Qt 按驱动配置采用 16.4 count/(°/s)、2048 count/g、`temp / 326.8 + 25` 的标称换算；没有姿态解算或个体标定。AP3216C 显示原始计数，不能直接当作 lux 或距离。

SR501 最终方案停用原来占用 GPIO1_IO01 的旧 DHT11/DS18B20 节点，并加入 GPIO1 引脚复用，见 [设备树说明](../device-tree/README.md)。本项目 DHT11 实际使用 GPIO1_IO02。
