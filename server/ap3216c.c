#include "ap3216c.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int read_sensor_value(const char *path, long *value)
{
    char text[64];
    size_t used = 0;

    if (path == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    for (;;) {
        /* sysfs 的一个计数值很短；缓冲填满时拒绝截断后的数据。 */
        if (used == sizeof(text) - 1) {
            close(fd);
            errno = EOVERFLOW;
            return -1;
        }

        ssize_t n = read(fd, text + used, sizeof(text) - 1 - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;

            int saved_errno = errno;
            close(fd);
            errno = saved_errno;
            return -1;
        }
        if (n == 0)
            break;

        used += (size_t)n;
    }

    if (close(fd) < 0)
        return -1;

    /* 拒绝空文件和嵌入的 NUL，避免只解析前半段。 */
    if (used == 0 || memchr(text, '\0', used) != NULL) {
        errno = EINVAL;
        return -1;
    }
    text[used] = '\0';

    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE)
        return -1;
    if (end == text || parsed < 0) {
        /* 驱动返回的文本 -1 也必须作为采样失败处理。 */
        errno = EINVAL;
        return -1;
    }

    while (isspace((unsigned char)*end))
        ++end;
    if (*end != '\0') {
        errno = EINVAL;
        return -1;
    }

    *value = parsed;
    return 0;
}

int ap3216c_read_sample(struct ap3216c_sample *sample)
{
    struct ap3216c_sample next = {0};

    if (sample == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 使用第03课已验证的文本接口；三次读取不构成同时刻快照。 */
    if (read_sensor_value("/sys/class/misc/ap3216c/ir", &next.ir) < 0 ||
        read_sensor_value("/sys/class/misc/ap3216c/als", &next.als) < 0 ||
        read_sensor_value("/sys/class/misc/ap3216c/ps", &next.ps) < 0)
        return -1;

    *sample = next;
    return 0;
}
