#include "dht11.h"
#include "descriptor_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

/* 只接受驱动的一位小数编码，不受locale影响，不接受符号和额外空白。 */
static int parse_tenths(const char **position, const char *end,
                        unsigned int maximum, unsigned int *value)
{
    const char *p = *position;
    unsigned int whole = 0;

    if (p == end || *p < '0' || *p > '9')
        return -1;
    while (p != end && *p >= '0' && *p <= '9') {
        whole = whole * 10 + (unsigned int)(*p++ - '0');
        /* 每步限制，既检查范围，也避免长数字导致整数溢出。 */
        if (whole > maximum / 10)
            return -1;
    }
    if (end - p < 2 || *p++ != '.' || *p < '0' || *p > '9')
        return -1;
    const unsigned int next = whole * 10 + (unsigned int)(*p++ - '0');
    if (next > maximum)
        return -1;
    *position = p;
    *value = next;
    return 0;
}

int dht11_read_value(const char *path, struct dht11_sample *sample)
{
    char text[32];
    size_t used = 0;
    struct dht11_sample next;

    if (path == NULL || sample == NULL) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    for (;;) {
        if (used == sizeof(text)) {
            close(fd);
            errno = EOVERFLOW;
            return -1;
        }
        const ssize_t n = read(fd, text + used, sizeof(text) - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            const int saved_errno = errno;
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

    const char *p = text;
    const char *end = text + used;
    if (parse_tenths(&p, end, 1000, &next.humidity_tenths) < 0 ||
        p == end || *p++ != ',' ||
        parse_tenths(&p, end, 500, &next.temperature_tenths) < 0)
        goto invalid;
    if (p != end && *p == '\n')
        ++p;
    if (p != end)
        goto invalid;

    /* 只有整条读取和解析成功后才交付结果。 */
    *sample = next;
    return 0;

invalid:
    errno = EINVAL;
    return -1;
}

int dht11_read_sample(struct dht11_sample *sample)
{
    return dht11_read_value("/sys/class/misc/vehicle_dht11/value", sample);
}
