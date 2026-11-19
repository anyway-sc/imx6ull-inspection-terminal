#include "icm20608.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

int icm20608_read_sample(struct icm20608_sample *sample)
{
    if (sample == NULL) {
        errno = EINVAL;
        return -1;
    }
    int fd = open("/dev/icm20608", O_RDONLY);
    if (fd < 0)
        return -1;

    int32_t raw[7];
    ssize_t n;
    do {
        n = read(fd, raw, sizeof(raw));
    } while (n < 0 && errno == EINTR);

    if (n != (ssize_t)sizeof(raw)) {
        int saved_error = n < 0 ? errno : EIO;
        close(fd);
        errno = saved_error;
        return -1;
    }
    if (close(fd) < 0)
        return -1;

    /* 不拼接短读：该驱动的下一次 read 会开始另一组采样。 */
    const struct icm20608_sample next = {
        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]
    };
    *sample = next;
    return 0;
}
