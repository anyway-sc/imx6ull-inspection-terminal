/*
 * 基于正点原子 22_spi/icm20608.c 的阶段03教学修正版。
 * 原作者：左忠凯；原版本：V1.0，2019/9/2。
 * Copyright © ALIENTEK Co., Ltd. 1998-2029. All rights reserved.
 *
 * 目标：本项目的一颗板载 ICM20608、Linux 4.1.15、ARMv7。
 * 用户接口：每次 read 返回 7 个有符号32位原始值，共28字节：
 * gx, gy, gz, ax, ay, az, temp；沿用本板小端字节序。
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <asm/uaccess.h>
#include "icm20608reg.h"

#define ICM20608_CNT  1
#define ICM20608_NAME "icm20608"
#define ICM20608_SPI_HZ 1000000U

struct icm20608_dev {
    dev_t devid;
    struct cdev cdev;
    struct class *class;
    struct device *device;
    int major;
    void *private_data;
    int legacy_cs_gpio;
    s32 gyro_x_adc;
    s32 gyro_y_adc;
    s32 gyro_z_adc;
    s32 accel_x_adc;
    s32 accel_y_adc;
    s32 accel_z_adc;
    s32 temp_adc;
};

static struct icm20608_dev icm20608dev;
static DEFINE_MUTEX(icm20608_lock);

/*
 * 本板旧DTB使用cs-gpio，而配套控制器驱动只解析cs-gpios。
 * 存在标准属性时交给控制器；仅在旧属性存在时兼容手动片选。
 * GPIO来自运行设备树，不硬编码引脚；devm在probe失败/解绑时释放。
 */
static int icm20608_setup_cs(struct spi_device *spi)
{
    struct device_node *np = spi->master->dev.of_node;
    int gpio;
    int ret;

    icm20608dev.legacy_cs_gpio = -1;
    if (!np || of_find_property(np, "cs-gpios", NULL))
        return 0;
    if (!of_find_property(np, "cs-gpio", NULL))
        return 0;

    gpio = of_get_named_gpio(np, "cs-gpio", spi->chip_select);
    if (!gpio_is_valid(gpio)) {
        dev_err(&spi->dev, "invalid legacy cs-gpio: %d\n", gpio);
        return gpio < 0 ? gpio : -EINVAL;
    }

    /* ICM20608的片选低有效，空闲状态先置高。 */
    ret = devm_gpio_request_one(&spi->dev, gpio, GPIOF_OUT_INIT_HIGH,
                                "icm20608-cs");
    if (ret) {
        dev_err(&spi->dev, "request legacy CS GPIO %d failed: %d\n",
                gpio, ret);
        return ret;
    }
    icm20608dev.legacy_cs_gpio = gpio;
    dev_info(&spi->dev, "legacy cs-gpio: using GPIO %d, active low\n", gpio);
    return 0;
}

static void icm20608_select(struct icm20608_dev *dev, bool selected)
{
    if (gpio_is_valid(dev->legacy_cs_gpio))
        gpio_set_value_cansleep(dev->legacy_cs_gpio, selected ? 0 : 1);
}

/* 返回状态；有效数据写入 buf。两个传输阶段保持片选有效。 */
static int icm20608_read_regs(struct icm20608_dev *dev,
                             u8 reg, void *buf, int len)
{
    struct spi_device *spi = dev->private_data;
    u8 command = reg | 0x80;
    int ret;

    if (!buf || len <= 0 || reg > 0x7f || len > 0x80 - reg)
        return -EINVAL;
    if (!spi)
        return -ENODEV;

    icm20608_select(dev, true);
    ret = spi_write_then_read(spi, &command, 1, buf, len);
    icm20608_select(dev, false);
    return ret;
}

/* 命令和数据放在同一块内核分配的缓冲区，避免越界及栈DMA问题。 */
static int icm20608_write_regs(struct icm20608_dev *dev,
                              u8 reg, const u8 *buf, int len)
{
    struct spi_device *spi = dev->private_data;
    u8 *txdata;
    int ret;

    if (!buf || len <= 0 || reg > 0x7f || len > 0x80 - reg)
        return -EINVAL;
    if (!spi)
        return -ENODEV;

    txdata = kmalloc(len + 1, GFP_KERNEL);
    if (!txdata)
        return -ENOMEM;

    txdata[0] = reg & 0x7f;
    memcpy(txdata + 1, buf, len);
    icm20608_select(dev, true);
    ret = spi_write(spi, txdata, len + 1);
    icm20608_select(dev, false);
    kfree(txdata);
    return ret;
}

static int icm20608_read_onereg(struct icm20608_dev *dev,
                              u8 reg, u8 *value)
{
    return icm20608_read_regs(dev, reg, value, 1);
}

static int icm20608_write_onereg(struct icm20608_dev *dev,
                               u8 reg, u8 value)
{
    return icm20608_write_regs(dev, reg, &value, 1);
}

/* 所有读写均检查返回值；未得到预期芯片ID时拒绝绑定。 */
static int icm20608_reginit(void)
{
    struct spi_device *spi = icm20608dev.private_data;
    u8 value;
    int ret;

    ret = icm20608_write_onereg(&icm20608dev, ICM20_PWR_MGMT_1, 0x80);
    if (ret)
        return ret;
    msleep(50);

    ret = icm20608_write_onereg(&icm20608dev, ICM20_PWR_MGMT_1, 0x01);
    if (ret)
        return ret;
    msleep(50);

    ret = icm20608_read_onereg(&icm20608dev, ICM20_WHO_AM_I, &value);
    if (ret)
        return ret;
    if (value != ICM20608G_ID && value != ICM20608D_ID) {
        dev_err(&spi->dev, "unexpected WHO_AM_I: %#x\n", value);
        return -ENODEV;
    }
    dev_info(&spi->dev, "ICM20608 ID = %#x\n", value);

    ret = icm20608_write_onereg(&icm20608dev, ICM20_SMPLRT_DIV, 0x00);
    if (ret)
        return ret;
    ret = icm20608_write_onereg(&icm20608dev, ICM20_GYRO_CONFIG, 0x18);
    if (ret)
        return ret;
    ret = icm20608_write_onereg(&icm20608dev, ICM20_ACCEL_CONFIG, 0x18);
    if (ret)
        return ret;
    ret = icm20608_write_onereg(&icm20608dev, ICM20_CONFIG, 0x04);
    if (ret)
        return ret;
    ret = icm20608_write_onereg(&icm20608dev, ICM20_ACCEL_CONFIG2, 0x04);
    if (ret)
        return ret;
    ret = icm20608_write_onereg(&icm20608dev, ICM20_PWR_MGMT_2, 0x00);
    if (ret)
        return ret;
    ret = icm20608_write_onereg(&icm20608dev, ICM20_LP_MODE_CFG, 0x00);
    if (ret)
        return ret;
    ret = icm20608_write_onereg(&icm20608dev, ICM20_FIFO_EN, 0x00);
    if (ret)
        return ret;

    /* 给刚启用的传感器留出启动时间。 */
    msleep(50);
    return 0;
}

static int icm20608_readdata(struct icm20608_dev *dev)
{
    u8 data[14];
    int ret;

    ret = icm20608_read_regs(dev, ICM20_ACCEL_XOUT_H, data, sizeof(data));
    if (ret)
        return ret;

    dev->accel_x_adc = (s16)((data[0] << 8) | data[1]);
    dev->accel_y_adc = (s16)((data[2] << 8) | data[3]);
    dev->accel_z_adc = (s16)((data[4] << 8) | data[5]);
    dev->temp_adc    = (s16)((data[6] << 8) | data[7]);
    dev->gyro_x_adc  = (s16)((data[8] << 8) | data[9]);
    dev->gyro_y_adc  = (s16)((data[10] << 8) | data[11]);
    dev->gyro_z_adc  = (s16)((data[12] << 8) | data[13]);
    return 0;
}

static int icm20608_open(struct inode *inode, struct file *filp)
{
    int ret;

    ret = mutex_lock_interruptible(&icm20608_lock);
    if (ret)
        return ret;
    if (!icm20608dev.private_data) {
        mutex_unlock(&icm20608_lock);
        return -ENODEV;
    }
    filp->private_data = &icm20608dev;
    mutex_unlock(&icm20608_lock);
    return nonseekable_open(inode, filp);
}

/* 连续采样接口：每次成功返回28，不采用文件偏移量表示读到末尾。 */
static ssize_t icm20608_read(struct file *filp, char __user *buf,
                           size_t cnt, loff_t *off)
{
    struct icm20608_dev *dev = filp->private_data;
    s32 data[7];
    int ret;

    (void)off;
    if (cnt == 0)
        return 0;
    if (cnt < sizeof(data))
        return -EINVAL;

    ret = mutex_lock_interruptible(&icm20608_lock);
    if (ret)
        return ret;

    ret = icm20608_readdata(dev);
    if (!ret) {
        data[0] = dev->gyro_x_adc;
        data[1] = dev->gyro_y_adc;
        data[2] = dev->gyro_z_adc;
        data[3] = dev->accel_x_adc;
        data[4] = dev->accel_y_adc;
        data[5] = dev->accel_z_adc;
        data[6] = dev->temp_adc;
    }
    mutex_unlock(&icm20608_lock);
    if (ret)
        return ret;

    if (copy_to_user(buf, data, sizeof(data)))
        return -EFAULT;
    return sizeof(data);
}

static const struct file_operations icm20608_ops = {
    .owner = THIS_MODULE,
    .open = icm20608_open,
    .read = icm20608_read,
    .llseek = no_llseek,
};

static int icm20608_probe(struct spi_device *spi)
{
    int ret;

    mutex_lock(&icm20608_lock);
    if (icm20608dev.private_data) {
        ret = -EBUSY;
        goto out_unlock;
    }

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    /* 初次硬件验证采用不超过1MHz的SPI时钟。 */
    if (!spi->max_speed_hz || spi->max_speed_hz > ICM20608_SPI_HZ)
        spi->max_speed_hz = ICM20608_SPI_HZ;
    ret = spi_setup(spi);
    if (ret)
        goto out_unlock;

    ret = icm20608_setup_cs(spi);
    if (ret)
        goto out_unlock;

    icm20608dev.private_data = spi;
    ret = icm20608_reginit();
    if (ret) {
        dev_err(&spi->dev, "initialization failed: %d\n", ret);
        goto err_clear_spi;
    }

    if (icm20608dev.major) {
        icm20608dev.devid = MKDEV(icm20608dev.major, 0);
        ret = register_chrdev_region(icm20608dev.devid,
                                    ICM20608_CNT, ICM20608_NAME);
    } else {
        ret = alloc_chrdev_region(&icm20608dev.devid, 0,
                                 ICM20608_CNT, ICM20608_NAME);
    }
    if (ret)
        goto err_clear_spi;
    icm20608dev.major = MAJOR(icm20608dev.devid);

    cdev_init(&icm20608dev.cdev, &icm20608_ops);
    icm20608dev.cdev.owner = THIS_MODULE;
    ret = cdev_add(&icm20608dev.cdev, icm20608dev.devid, ICM20608_CNT);
    if (ret)
        goto err_region;

    icm20608dev.class = class_create(THIS_MODULE, ICM20608_NAME);
    if (IS_ERR(icm20608dev.class)) {
        ret = PTR_ERR(icm20608dev.class);
        goto err_cdev;
    }
    icm20608dev.device = device_create(icm20608dev.class, NULL,
                                     icm20608dev.devid, NULL,
                                     ICM20608_NAME);
    if (IS_ERR(icm20608dev.device)) {
        ret = PTR_ERR(icm20608dev.device);
        goto err_class;
    }

    spi_set_drvdata(spi, &icm20608dev);
    dev_info(&spi->dev, "character device ready, sample size = 28 bytes\n");
    mutex_unlock(&icm20608_lock);
    return 0;

/* 从失败位置进入对应标签，依次释放此前成功申请的资源。 */
err_class:
    class_destroy(icm20608dev.class);
err_cdev:
    cdev_del(&icm20608dev.cdev);
err_region:
    unregister_chrdev_region(icm20608dev.devid, ICM20608_CNT);
err_clear_spi:
    icm20608dev.private_data = NULL;
out_unlock:
    mutex_unlock(&icm20608_lock);
    return ret;
}

static int icm20608_remove(struct spi_device *spi)
{
    struct icm20608_dev *dev = spi_get_drvdata(spi);

    mutex_lock(&icm20608_lock);
    icm20608_select(dev, false);
    dev->private_data = NULL;
    device_destroy(dev->class, dev->devid);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devid, ICM20608_CNT);
    spi_set_drvdata(spi, NULL);
    mutex_unlock(&icm20608_lock);
    return 0;
}

/* SPI modalias 是 spi:icm20608；设备树匹配使用完整 compatible。 */
static const struct spi_device_id icm20608_id[] = {
    { "icm20608", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, icm20608_id);

static const struct of_device_id icm20608_of_match[] = {
    { .compatible = "alientek,icm20608" },
    { }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_driver = {
    .probe = icm20608_probe,
    .remove = icm20608_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name = ICM20608_NAME,
        .of_match_table = icm20608_of_match,
    },
    .id_table = icm20608_id,
};

static int __init icm20608_init(void)
{
    return spi_register_driver(&icm20608_driver);
}

static void __exit icm20608_exit(void)
{
    spi_unregister_driver(&icm20608_driver);
}

module_init(icm20608_init);
module_exit(icm20608_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("zuozhongkai");
MODULE_DESCRIPTION("ICM20608 stage03 SPI character driver");
MODULE_VERSION("stage03-csfix1");
