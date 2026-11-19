#include "sr04.h"
#include "descriptor_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

static int parse_unsigned(const char **position, const char *end,
                           unsigned int maximum, unsigned int *value)
{
    const char *p = *position;
    unsigned int number = 0;

    if (p == end || *p < '0' || *p > '9')
        return -1;
    while (p != end && *p >= '0' && *p <= '9') {
        const unsigned int digit = (unsigned int)(*p++ - '0');
        if (number > (maximum - digit) / 10)
            return -1;
        number = number * 10 + digit;
        if (number > maximum)
            return -1;
    }
    *position = p;
    *value = number;
    return 0;
}

int sr04_read_value(const char *path, struct sr04_sample *sample)
{
    char text[32];
    size_t used = 0;
    struct sr04_sample next;

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
    if (parse_unsigned(&p, end, 4000, &next.distance_mm) < 0 ||
        next.distance_mm < 20 || p == end || *p++ != ',' ||
        parse_unsigned(&p, end, 40000, &next.pulse_us) < 0 || next.pulse_us == 0)
        goto invalid;
    if (p != end && *p == '\n')
        ++p;
    if (p != end)
        goto invalid;

    /* 驱动用纳秒换算，输出脉宽舍入为微秒，允许最多1mm舍入差。 */
    const unsigned int expected_mm = (next.pulse_us * 343U + 1000U) / 2000U;
    if (next.distance_mm + 1 < expected_mm || expected_mm + 1 < next.distance_mm)
        goto invalid;
    *sample = next;
    return 0;

invalid:
    errno = EINVAL;
    return -1;
}

int sr04_read_sample(struct sr04_sample *sample)
{
    return sr04_read_value("/sys/class/misc/vehicle_sr04/value", sample);
}
