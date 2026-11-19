#ifndef VEHICLE_SENSOR_SAMPLER_H
#define VEHICLE_SENSOR_SAMPLER_H

#include "ap3216c.h"
#include "dht11.h"
#include "icm20608.h"
#include "sr04.h"
#include <time.h>

enum sensor_kind {
    SENSOR_AP3216C, SENSOR_ICM20608, SENSOR_DHT11,
    SENSOR_SR04, SENSOR_SR501, SENSOR_COUNT
};

union sensor_value {
    struct ap3216c_sample ap3216c;
    struct icm20608_sample icm20608;
    struct dht11_sample dht11;
    struct sr04_sample sr04;
    unsigned int sr501;
};

struct sensor_snapshot {
    union sensor_value value;
    struct timespec completed_at; /* 一次读取完成的单调时间，不是硬件同步采样时间。 */
    unsigned long sequence;
};

struct sensor_policy {
    unsigned int interval_ms; /* 每次读取结束后等待的时间。 */
    unsigned int max_age_ms;
};

struct sensor_sampler;
/* policies 为 NULL 使用默认值，否则必须提供 SENSOR_COUNT 项。失败自动回收。 */
struct sensor_sampler *sensor_sampler_create(const struct sensor_policy *policies);
/* 只复制最新成功快照；未就绪、最新读取失败或过期均返回 -1，输出不变。 */
int sensor_sampler_get(struct sensor_sampler *sampler, enum sensor_kind kind,
                       struct sensor_snapshot *snapshot);
/* 先通知全部线程退出，再由 destroy join；不强行取消正在执行的设备读取。 */
void sensor_sampler_request_stop(struct sensor_sampler *sampler);
void sensor_sampler_destroy(struct sensor_sampler *sampler);

#endif
