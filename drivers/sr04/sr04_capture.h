/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VEHICLE_SR04_CAPTURE_H
#define VEHICLE_SR04_CAPTURE_H

#ifdef __KERNEL__
#include <linux/math64.h>
#endif

#define VEHICLE_SR04_TIMEOUT_NS 40000000ULL

enum vehicle_sr04_phase {
    VEHICLE_SR04_IDLE,
    VEHICLE_SR04_WAIT_RISE,
    VEHICLE_SR04_WAIT_FALL,
    VEHICLE_SR04_DONE,
    VEHICLE_SR04_TIMED_OUT
};

struct vehicle_sr04_capture {
    enum vehicle_sr04_phase phase;
    unsigned long long armed_ns;
    unsigned long long deadline_ns;
    unsigned long long rise_ns;
    unsigned long long pulse_ns;
};

struct vehicle_sr04_sample {
    unsigned int distance_mm;
    unsigned int pulse_us;
};

/* 调用方负责锁；只有当前采样窗口中的先升后降才能完成一次测量。 */
static inline void vehicle_sr04_begin(struct vehicle_sr04_capture *capture,
                                      unsigned long long now_ns)
{
    capture->phase = VEHICLE_SR04_WAIT_RISE;
    capture->armed_ns = now_ns;
    capture->deadline_ns = now_ns + VEHICLE_SR04_TIMEOUT_NS;
    capture->rise_ns = 0;
    capture->pulse_ns = 0;
}

static inline void vehicle_sr04_cancel(struct vehicle_sr04_capture *capture)
{
    capture->phase = VEHICLE_SR04_IDLE;
}

/* 返回1表示本次采样完成或超时，需要唤醒读取者。 */
static inline int vehicle_sr04_edge(struct vehicle_sr04_capture *capture,
                                    int high, unsigned long long now_ns)
{
    if (capture->phase != VEHICLE_SR04_WAIT_RISE &&
        capture->phase != VEHICLE_SR04_WAIT_FALL)
        return 0;
    if (now_ns < capture->armed_ns)
        return 0;
    if (now_ns >= capture->deadline_ns) {
        capture->phase = VEHICLE_SR04_TIMED_OUT;
        return 1;
    }
    if (capture->phase == VEHICLE_SR04_WAIT_RISE) {
        if (high) {
            capture->rise_ns = now_ns;
            capture->phase = VEHICLE_SR04_WAIT_FALL;
        }
        return 0;
    }
    if (!high && now_ns >= capture->rise_ns) {
        capture->pulse_ns = now_ns - capture->rise_ns;
        capture->phase = VEHICLE_SR04_DONE;
        return 1;
    }
    return 0;
}

static inline unsigned long long vehicle_sr04_div_u64(
    unsigned long long value, unsigned int divisor)
{
#ifdef __KERNEL__
    return div_u64(value, divisor);
#else
    return value / divisor;
#endif
}

/* 采用343m/s名义声速，往返时间除以2；失败时保留调用方原输出。 */
static inline int vehicle_sr04_convert(unsigned long long pulse_ns,
                                       struct vehicle_sr04_sample *sample)
{
    unsigned long long distance_mm;
    unsigned int pulse_us;

    if (!pulse_ns || pulse_ns > VEHICLE_SR04_TIMEOUT_NS)
        return -1;
    distance_mm = vehicle_sr04_div_u64(pulse_ns * 343ULL + 1000000ULL,
                                       2000000U);
    if (distance_mm < 20 || distance_mm > 4000)
        return -1;
    pulse_us = (unsigned int)vehicle_sr04_div_u64(pulse_ns + 500ULL, 1000U);
    sample->distance_mm = (unsigned int)distance_mm;
    sample->pulse_us = pulse_us;
    return 0;
}

#endif
