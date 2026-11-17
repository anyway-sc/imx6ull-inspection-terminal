#include "sr501.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int read_text(const char *path, char *text, size_t capacity, size_t *length)
{
    int fd = open(path, O_RDONLY);
    size_t used = 0;
    if (fd < 0)
        return -1;
    for (;;) {
        if (used == capacity) {
            close(fd);
            errno = EOVERFLOW;
            return -1;
        }
        const ssize_t n = read(fd, text + used, capacity - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            const int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    if (close(fd) < 0)
        return -1;
    *length = used;
    return 0;
}

int sr501_read_value(const char *path, unsigned int *level)
{
    char text[8];
    size_t length;
    if (path == NULL || level == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (read_text(path, text, sizeof(text), &length) < 0)
        return -1;
    if ((length != 1 && (length != 2 || text[1] != '\n')) ||
        (text[0] != '0' && text[0] != '1')) {
        errno = EINVAL;
        return -1;
    }
    *level = (unsigned int)(text[0] - '0');
    return 0;
}

int sr501_read_sample(unsigned int *level)
{
    char direction[8];
    size_t length;
    unsigned int inverted;
    if (level == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (read_text("/sys/class/gpio/gpio1/direction", direction,
                  sizeof(direction), &length) < 0)
        return -1;
    if (length != 3 || memcmp(direction, "in\n", 3) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (sr501_read_value("/sys/class/gpio/gpio1/active_low", &inverted) < 0)
        return -1;
    if (inverted != 0) {
        errno = EINVAL;
        return -1;
    }
    return sr501_read_value("/sys/class/gpio/gpio1/value", level);
}
