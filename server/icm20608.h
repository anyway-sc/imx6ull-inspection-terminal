#ifndef ICM20608_H
#define ICM20608_H

#include <stdint.h>

struct icm20608_sample {
    int32_t gx, gy, gz;
    int32_t ax, ay, az;
    int32_t temp;
};

/* 已验收驱动每次 read 交付7个 int32_t；失败时不改输出。 */
int icm20608_read_sample(struct icm20608_sample *sample);

#endif
