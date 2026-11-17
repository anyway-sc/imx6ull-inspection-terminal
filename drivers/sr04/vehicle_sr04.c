// SPDX-License-Identifier: GPL-2.0
/* ALPHA V2.8 / Linux 4.1.15: Trig=GPIO1_IO04, Echo=GPIO1_IO09. */
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
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/preempt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "sr04_capture.h"

#define SR04_VERSION "stage05-gpio4-9-v1"
#define SR04_TRIG_GPIO 4
#define SR04_ECHO_GPIO 9
#define SR04_GPIO1_BASE 0x0209c000
#define SR04_IOMUX_BASE 0x020e0000
#define SR04_TRIG_MUX_OFFSET 0x6c
#define SR04_ECHO_MUX_OFFSET 0x80
#define SR04_GPIO_MUX_MODE 5
#define SR04_INTERVAL_MS 100
#define SR04_WAIT_MS 40

struct sr04_device {
    struct mutex sample_lock;
    spinlock_t edge_lock;
    struct completion complete;
    struct vehicle_sr04_capture capture;
    unsigned long next_trigger;
    int irq;
};

struct sr04_file {
    struct mutex lock;
    char text[32];
    int length;
};

static struct sr04_device sr04;

static unsigned long long now_ns(void)
{
    return (unsigned long long)ktime_to_ns(ktime_get());
}

static int check_gpio_mapping(void)
{
    struct device_node *node;
    struct resource resource;
    void __iomem *mux;
    u32 trig_mode, echo_mode;
    int ret;

    node = of_find_node_by_path(
        "/soc/aips-bus@02000000/gpio@0209c000");
    if (!node)
        return -ENODEV;
    ret = of_address_to_resource(node, 0, &resource);
    if (!ret && (resource.start != SR04_GPIO1_BASE ||
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
    if (resource.start != SR04_IOMUX_BASE ||
        resource_size(&resource) < SR04_ECHO_MUX_OFFSET + sizeof(u32))
        return -ENODEV;

    /* 复用寄存器仅只读检查，模式不符时由用户提供日志再决定适配。 */
    mux = ioremap(resource.start + SR04_TRIG_MUX_OFFSET,
                  SR04_ECHO_MUX_OFFSET - SR04_TRIG_MUX_OFFSET + sizeof(u32));
    if (!mux)
        return -ENOMEM;
    trig_mode = readl(mux);
    echo_mode = readl(mux + SR04_ECHO_MUX_OFFSET - SR04_TRIG_MUX_OFFSET);
    iounmap(mux);
    pr_info("vehicle_sr04: GPIO1_IO04 mux=0x%08x, GPIO1_IO09 mux=0x%08x (read only)\n",
            trig_mode, echo_mode);
    if ((trig_mode & 0x0f) != SR04_GPIO_MUX_MODE ||
        (echo_mode & 0x0f) != SR04_GPIO_MUX_MODE) {
        pr_err("vehicle_sr04: pin is not in GPIO mode; no mux change made\n");
        return -EINVAL;
    }
    return 0;
}

static irqreturn_t sr04_echo_irq(int irq, void *context)
{
    struct sr04_device *device = context;
    unsigned long long stamp = now_ns();
    int high = !!gpio_get_value(SR04_ECHO_GPIO);
    unsigned long flags;

    (void)irq;
    spin_lock_irqsave(&device->edge_lock, flags);
    if (vehicle_sr04_edge(&device->capture, high, stamp))
        complete(&device->complete);
    spin_unlock_irqrestore(&device->edge_lock, flags);
    return IRQ_HANDLED;
}

static int read_sample(struct vehicle_sr04_sample *sample)
{
    unsigned long long pulse_ns = 0;
    unsigned long flags;
    long waited;
    int ret;

    if (mutex_lock_interruptible(&sr04.sample_lock))
        return -ERESTARTSYS;

    for (;;) {
        unsigned long now = jiffies;
        unsigned int remaining;

        if (!time_before(now, sr04.next_trigger))
            break;
        remaining = jiffies_to_msecs(sr04.next_trigger - now) + 1;
        if (msleep_interruptible(remaining)) {
            ret = -ERESTARTSYS;
            goto unlock;
        }
    }

    /* 锁覆盖限频与完整采样，失败也不允许紧密重试。 */
    sr04.next_trigger = jiffies + msecs_to_jiffies(SR04_INTERVAL_MS) + 1;
    gpio_set_value(SR04_TRIG_GPIO, 0);
    udelay(2);
    spin_lock_irqsave(&sr04.edge_lock, flags);
    if (gpio_get_value(SR04_ECHO_GPIO)) {
        spin_unlock_irqrestore(&sr04.edge_lock, flags);
        ret = -EBUSY;
        goto unlock;
    }
    /* 与IRQ中的complete共用同一把锁，防止取消后旧完成量污染下次读取。 */
    reinit_completion(&sr04.complete);
    vehicle_sr04_begin(&sr04.capture, now_ns());
    spin_unlock_irqrestore(&sr04.edge_lock, flags);

    /* 仅触发脉冲期间禁止任务抢占，中断保持开启。 */
    preempt_disable();
    gpio_set_value(SR04_TRIG_GPIO, 1);
    udelay(12);
    gpio_set_value(SR04_TRIG_GPIO, 0);
    /* 从实际脉冲结束重新限频，调度延迟不会压缩下次触发间隔。 */
    sr04.next_trigger = jiffies + msecs_to_jiffies(SR04_INTERVAL_MS) + 1;
    preempt_enable();

    waited = wait_for_completion_interruptible_timeout(
        &sr04.complete, msecs_to_jiffies(SR04_WAIT_MS) + 1);
    spin_lock_irqsave(&sr04.edge_lock, flags);
    if (waited < 0) {
        ret = -ERESTARTSYS;
    } else if (sr04.capture.phase == VEHICLE_SR04_DONE) {
        pulse_ns = sr04.capture.pulse_ns;
        ret = 0;
    } else {
        ret = -ETIMEDOUT;
    }
    vehicle_sr04_cancel(&sr04.capture);
    spin_unlock_irqrestore(&sr04.edge_lock, flags);
    if (!ret && vehicle_sr04_convert(pulse_ns, sample))
        ret = -ERANGE;

unlock:
    mutex_unlock(&sr04.sample_lock);
    return ret;
}

static int format_sample(char *text, size_t size,
                         const struct vehicle_sr04_sample *sample)
{
    return scnprintf(text, size, "%u,%u\n",
                     sample->distance_mm, sample->pulse_us);
}

static ssize_t value_show(struct device *dev,
                          struct device_attribute *attr, char *buf)
{
    struct vehicle_sr04_sample sample;
    int ret;

    (void)dev;
    (void)attr;
    ret = read_sample(&sample);
    if (ret)
        return ret;
    return format_sample(buf, PAGE_SIZE, &sample);
}

static DEVICE_ATTR_RO(value);

static struct attribute *sr04_attrs[] = {
    &dev_attr_value.attr,
    NULL
};

static const struct attribute_group sr04_group = {
    .attrs = sr04_attrs
};

static const struct attribute_group *sr04_groups[] = {
    &sr04_group,
    NULL
};

static int sr04_open(struct inode *inode, struct file *file)
{
    struct sr04_file *snapshot;
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

static ssize_t sr04_read(struct file *file, char __user *buf,
                         size_t count, loff_t *position)
{
    struct sr04_file *snapshot = file->private_data;
    struct vehicle_sr04_sample sample;
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

static int sr04_release(struct inode *inode, struct file *file)
{
    (void)inode;
    kfree(file->private_data);
    return 0;
}

static const struct file_operations sr04_fops = {
    .owner = THIS_MODULE,
    .open = sr04_open,
    .read = sr04_read,
    .release = sr04_release,
    .llseek = no_llseek
};

static struct miscdevice sr04_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "vehicle_sr04",
    .fops = &sr04_fops,
    .groups = sr04_groups
};

static int __init vehicle_sr04_init(void)
{
    int ret;

    ret = check_gpio_mapping();
    if (ret)
        return ret;
    if (!gpio_is_valid(SR04_TRIG_GPIO) || !gpio_is_valid(SR04_ECHO_GPIO) ||
        gpio_cansleep(SR04_TRIG_GPIO) || gpio_cansleep(SR04_ECHO_GPIO))
        return -EOPNOTSUPP;

    mutex_init(&sr04.sample_lock);
    spin_lock_init(&sr04.edge_lock);
    init_completion(&sr04.complete);
    vehicle_sr04_cancel(&sr04.capture);

    ret = gpio_request(SR04_TRIG_GPIO, "vehicle-sr04-trig");
    if (ret)
        return ret;
    ret = gpio_request(SR04_ECHO_GPIO, "vehicle-sr04-echo");
    if (ret)
        goto free_trig;
    ret = gpio_direction_input(SR04_ECHO_GPIO);
    if (ret)
        goto free_echo;
    ret = gpio_direction_output(SR04_TRIG_GPIO, 0);
    if (ret)
        goto free_echo;

    sr04.irq = gpio_to_irq(SR04_ECHO_GPIO);
    if (sr04.irq < 0) {
        ret = sr04.irq;
        goto free_echo;
    }
    ret = request_irq(sr04.irq, sr04_echo_irq,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_THREAD,
                      "vehicle-sr04-echo", &sr04);
    if (ret)
        goto free_echo;

    sr04.next_trigger = jiffies + msecs_to_jiffies(SR04_INTERVAL_MS) + 1;
    ret = misc_register(&sr04_misc);
    if (ret)
        goto free_echo_irq;
    pr_info("vehicle_sr04: %s ready, Trig=GPIO1_IO04, Echo=GPIO1_IO09, irq=%d; read distance_mm,pulse_us\n",
            SR04_VERSION, sr04.irq);
    return 0;

free_echo_irq:
    free_irq(sr04.irq, &sr04);
free_echo:
    gpio_direction_input(SR04_ECHO_GPIO);
    gpio_free(SR04_ECHO_GPIO);
free_trig:
    gpio_direction_input(SR04_TRIG_GPIO);
    gpio_free(SR04_TRIG_GPIO);
    return ret;
}

static void __exit vehicle_sr04_exit(void)
{
    unsigned long flags;

    misc_deregister(&sr04_misc);
    spin_lock_irqsave(&sr04.edge_lock, flags);
    vehicle_sr04_cancel(&sr04.capture);
    spin_unlock_irqrestore(&sr04.edge_lock, flags);
    free_irq(sr04.irq, &sr04);
    gpio_set_value(SR04_TRIG_GPIO, 0);
    gpio_direction_input(SR04_TRIG_GPIO);
    gpio_direction_input(SR04_ECHO_GPIO);
    gpio_free(SR04_ECHO_GPIO);
    gpio_free(SR04_TRIG_GPIO);
    pr_info("vehicle_sr04: released GPIO1_IO04 and GPIO1_IO09\n");
}

module_init(vehicle_sr04_init);
module_exit(vehicle_sr04_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vehicle learning project");
MODULE_DESCRIPTION("ALPHA V2.8 HC-SR04-RC GPIO4/9 edge capture, no DTB or IOMUX writes");
MODULE_VERSION(SR04_VERSION);
