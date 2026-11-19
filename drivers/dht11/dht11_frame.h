/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VEHICLE_DHT11_FRAME_H
#define VEHICLE_DHT11_FRAME_H

/* DHT11 常见格式：湿度整数/小数、温度整数/小数、低8位校验和。 */
struct vehicle_dht11_sample {
    unsigned int humidity_tenths;
    unsigned int temperature_tenths;
};

enum vehicle_dht11_frame_result {
    VEHICLE_DHT11_FRAME_OK = 0,
    VEHICLE_DHT11_FRAME_CHECKSUM = -1,
    VEHICLE_DHT11_FRAME_FORMAT = -2
};

static inline int vehicle_dht11_decode_frame(
    const unsigned char raw[5], struct vehicle_dht11_sample *sample)
{
    struct vehicle_dht11_sample next;
    unsigned char checksum;

    checksum = (unsigned char)(raw[0] + raw[1] + raw[2] + raw[3]);
    if (checksum != raw[4])
        return VEHICLE_DHT11_FRAME_CHECKSUM;

    /* 面向DHT11，不把DHT22编码或非法小数当作本模块读数。 */
    if (raw[1] > 9 || raw[3] > 9)
        return VEHICLE_DHT11_FRAME_FORMAT;
    next.humidity_tenths = raw[0] * 10u + raw[1];
    next.temperature_tenths = raw[2] * 10u + raw[3];
    if (next.humidity_tenths > 1000 || next.temperature_tenths > 500)
        return VEHICLE_DHT11_FRAME_FORMAT;

    *sample = next;
    return VEHICLE_DHT11_FRAME_OK;
}

#endif
