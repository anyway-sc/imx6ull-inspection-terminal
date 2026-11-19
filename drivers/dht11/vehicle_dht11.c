// SPDX-License-Identifier: GPL-2.0
/*
 * ALPHA V2.8 / i.MX6ULL / Linux 4.1.15: DHT11 on GPIO1_IO02.
 * 独立实验模块，不绑定原设备树的 /dht11，不修改内核或DTB文件。
 * 只读检查IOMUX：若不是GPIO模式则拒绝加载，不写复用/电气配置寄存器。
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "dht11_frame.h"

#define DHT11_VERSION "stage05-gpio2-v1"
#define DHT11_GPIO1_BASE 0x0209c000
#define DHT11_IOMUX_BASE 0x020e0000
#define DHT11_MUX_OFFSET 0x64
#define DHT11_GPIO_MUX_MODE 5
#define DHT11_INTERVAL_MS 2100
#define DHT11_FRAME_LIMIT_US 6000

static int data_gpio = 2;
module_param(data_gpio, int, 0444);
MODULE_PARM_DESC(data_gpio, "GPIO1_IO02 only: global GPIO 2 on this ALPHA V2.8");

static DEFINE_MUTEX(sample_lock);
static unsigned long next_sample;

/* 每次open保留一份文本，短read不会导致同一文件重新触发硬件采样。 */
struct dht11_file {
    struct mutex lock;
    char text[32];
    int length;
};

static int check_gpio2_mapping(void)
{
    struct device_node *node;
    struct resource resource;
    void __iomem *mux;
    u32 mode;
    int ret;

    if (data_gpio != 2)
        return -EINVAL;

    node = of_find_node_by_path(
        "/soc/aips-bus@02000000/gpio@0209c000");
    if (!node)
        return -ENODEV;
    ret = of_address_to_resource(node, 0, &resource);
    if (!ret && (resource.start != DHT11_GPIO1_BASE ||
                 of_alias_get_id(node, "gpio") != 0))
        ret = -ENODEV;
    of_node_put(node);
    if (ret)
        return ret;

    node = of_find_node_by_path(
        "/soc/aips-bus@02000000/iomuxc@020e0000");
    if (!node)
        return -ENODEV;
    ret = of_address_to_resource(node, 0, &resource);
    of_node_put(node);
    if (ret)
        return ret;
    if (resource.start != DHT11_IOMUX_BASE ||
        resource_size(&resource) < DHT11_MUX_OFFSET + sizeof(u32))
        return -ENODEV;

    /* IOMUXC由原pinctrl驱动管理；这里只读已核对的一个寄存器。 */
    mux = ioremap(resource.start + DHT11_MUX_OFFSET, sizeof(u32));
    if (!mux)
        return -ENOMEM;
    mode = readl(mux);
    iounmap(mux);
    pr_info("vehicle_dht11: GPIO1_IO02 mux=0x%08x (read only)\n", mode);
    if ((mode & 0x0f) != DHT11_GPIO_MUX_MODE) {
        pr_err("vehicle_dht11: GPIO1_IO02 is not in GPIO mode; no mux change made\n");
        return -EINVAL;
    }
    return 0;
}

static s64 now_ns(void)
{
    return ktime_to_ns(ktime_get());
}

/* 每次等待和整帧均有截止时间，断线/短路不会造成无限忙等。 */
static int wait_level(int level, unsigned int timeout_us, s64 frame_deadline)
{
    s64 deadline = now_ns() + (s64)timeout_us * NSEC_PER_USEC;

    while (gpio_get_value(data_gpio) != level) {
        s64 now = now_ns();

        if (now >= deadline || now >= frame_deadline)
            return -ETIMEDOUT;
        cpu_relax();
    }
    return now_ns() < frame_deadline ? 0 : -ETIMEDOUT;
}

static int capture_frame(unsigned char raw[5])
{
    unsigned long flags;
    s64 frame_deadline;
    int bit;
    int ret;

    memset(raw, 0, 5);
    ret = gpio_direction_output(data_gpio, 0);
    if (ret)
        return ret;

    /* 主机低电平至少18ms；此阶段允许调度和中断。 */
    if (msleep_interruptible(20)) {
        gpio_direction_input(data_gpio);
        return -ERESTARTSYS;
    }

    /* 只在微秒接收阶段屏蔽本CPU中断，整帧上限6ms。 */
    local_irq_save(flags);
    frame_deadline = now_ns() + (s64)DHT11_FRAME_LIMIT_US * NSEC_PER_USEC;
    ret = gpio_direction_input(data_gpio); /* 释放DATA，由模块上拉拉高。 */
    if (ret)
        goto restore_irqs;

    ret = wait_level(0, 120, frame_deadline); /* 传感器响应低电平。 */
    if (ret)
        goto restore_irqs;
    ret = wait_level(1, 120, frame_deadline);
    if (ret)
        goto restore_irqs;
    ret = wait_level(0, 120, frame_deadline);
    if (ret)
        goto restore_irqs;

    for (bit = 0; bit < 40; ++bit) {
        s64 high_start;
        s64 high_width;

        ret = wait_level(1, 120, frame_deadline);
        if (ret)
            goto restore_irqs;
        high_start = now_ns();
        ret = wait_level(0, 120, frame_deadline);
        if (ret)
            goto restore_irqs;
        high_width = now_ns() - high_start;
        raw[bit / 8] <<= 1;
        /* DHT11的0约26~28us，1约70us；50us作为判决点。 */
        if (high_width > 50 * NSEC_PER_USEC)
            raw[bit / 8] |= 1;
    }

restore_irqs:
    local_irq_restore(flags);
    /* 包括失败在内，结束时只保留输入，不主动输出高电平。 */
    if (gpio_direction_input(data_gpio) && !ret)
        ret = -EIO;
    return ret;
}

static int read_sample(struct vehicle_dht11_sample *sample)
{
    unsigned char raw[5];
    int ret;

    if (mutex_lock_interruptible(&sample_lock))
        return -ERESTARTSYS;

    for (;;) {
        unsigned long now = jiffies;
        unsigned int remaining;

        if (!time_before(now, next_sample))
            break;
        remaining = jiffies_to_msecs(next_sample - now) + 1;
        if (msleep_interruptible(remaining)) {
            ret = -ERESTARTSYS;
            goto unlock;
        }
    }
    /* 失败也限制下一次物理采样，避免紧密重试。 */
    next_sample = jiffies + msecs_to_jiffies(DHT11_INTERVAL_MS);
    ret = capture_frame(raw);
    if (ret)
        goto unlock;
    ret = vehicle_dht11_decode_frame(raw, sample);
    if (ret == VEHICLE_DHT11_FRAME_CHECKSUM)
        ret = -EBADMSG;
    else if (ret == VEHICLE_DHT11_FRAME_FORMAT)
        ret = -ERANGE;

unlock:
    mutex_unlock(&sample_lock);
    return ret;
}

static int format_sample(char *text, size_t size,
                         const struct vehicle_dht11_sample *sample)
{
    return scnprintf(text, size, "%u.%u,%u.%u\n",
                     sample->humidity_tenths / 10,
                     sample->humidity_tenths % 10,
                     sample->temperature_tenths / 10,
                     sample->temperature_tenths % 10);
}

static ssize_t value_show(struct device *dev,
                          struct device_attribute *attr, char *buf)
{
    struct vehicle_dht11_sample sample;
    int ret;

    (void)dev;
    (void)attr;
    ret = read_sample(&sample);
    if (ret)
        return ret;
    return format_sample(buf, PAGE_SIZE, &sample);
}

static DEVICE_ATTR_RO(value);

static struct attribute *dht11_attrs[] = {
    &dev_attr_value.attr,
    NULL
};

static const struct attribute_group dht11_group = {
    .attrs = dht11_attrs
};

static const struct attribute_group *dht11_groups[] = {
    &dht11_group,
    NULL
};

static int dht11_open(struct inode *inode, struct file *file)
{
    struct dht11_file *snapshot;
    int ret;

    snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
    if (!snapshot)
        return -ENOMEM;
    mutex_init(&snapshot->lock);
    ret = nonseekable_open(inode, file);
    if (ret) {
        kfree(snapshot);
        return ret;
    }
    file->private_data = snapshot;
    return 0;
}

static ssize_t dht11_read(struct file *file, char __user *buf,
                          size_t count, loff_t *position)
{
    struct dht11_file *snapshot = file->private_data;
    struct vehicle_dht11_sample sample;
    ssize_t result;
    int ret;

    if (!count)
        return 0;
    if (mutex_lock_interruptible(&snapshot->lock))
        return -ERESTARTSYS;
    if (!snapshot->length) {
        ret = read_sample(&sample);
        if (ret) {
            mutex_unlock(&snapshot->lock);
            return ret;
        }
        snapshot->length = format_sample(snapshot->text,
                                          sizeof(snapshot->text), &sample);
    }
    result = simple_read_from_buffer(buf, count, position,
                                     snapshot->text, snapshot->length);
    mutex_unlock(&snapshot->lock);
    return result;
}

static int dht11_release(struct inode *inode, struct file *file)
{
    (void)inode;
    kfree(file->private_data);
    return 0;
}

static const struct file_operations dht11_fops = {
    .owner = THIS_MODULE,
    .open = dht11_open,
    .read = dht11_read,
    .release = dht11_release,
    .llseek = no_llseek
};

static struct miscdevice dht11_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "vehicle_dht11",
    .fops = &dht11_fops,
    .groups = dht11_groups
};

static int __init vehicle_dht11_init(void)
{
    int ret;

    ret = check_gpio2_mapping();
    if (ret)
        return ret;
    if (!gpio_is_valid(data_gpio) || gpio_cansleep(data_gpio))
        return -EOPNOTSUPP;
    ret = gpio_request(data_gpio, "vehicle-dht11");
    if (ret)
        return ret;
    ret = gpio_direction_input(data_gpio);
    if (ret)
        goto free_gpio;

    next_sample = jiffies + msecs_to_jiffies(DHT11_INTERVAL_MS);
    ret = misc_register(&dht11_misc);
    if (ret)
        goto free_gpio;
    pr_info("vehicle_dht11: %s ready, DATA=GPIO1_IO02; read humidity,temperature\n",
            DHT11_VERSION);
    return 0;

free_gpio:
    gpio_direction_input(data_gpio);
    gpio_free(data_gpio);
    return ret;
}

static void __exit vehicle_dht11_exit(void)
{
    misc_deregister(&dht11_misc);
    gpio_direction_input(data_gpio);
    gpio_free(data_gpio);
    pr_info("vehicle_dht11: released GPIO1_IO02\n");
}

module_init(vehicle_dht11_init);
module_exit(vehicle_dht11_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vehicle learning project");
MODULE_DESCRIPTION("ALPHA V2.8 DHT11 on GPIO1_IO02, no DTB or IOMUX writes");
MODULE_VERSION(DHT11_VERSION);
