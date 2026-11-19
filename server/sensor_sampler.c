#include "sensor_sampler.h"
#include "sr501.h"
#include "thread_util.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct sensor_cache {
    struct sensor_snapshot snapshot;
    int ready;
    int error;
};

struct sampler_argument {
    struct sensor_sampler *sampler;
    enum sensor_kind kind;
};

struct sensor_sampler {
    pthread_mutex_t mutex;
    pthread_mutex_t timing_mutex;
    pthread_cond_t changed;
    int stopping;
    unsigned int started;
    pthread_t threads[SENSOR_COUNT];
    struct sampler_argument arguments[SENSOR_COUNT];
    struct sensor_policy policies[SENSOR_COUNT];
    struct sensor_cache caches[SENSOR_COUNT];
};

static const struct sensor_policy default_policies[SENSOR_COUNT] = {
    {1000, 3000}, {1000, 3000}, {3000, 7000}, {500, 2000}, {500, 2000}
};

static int read_sensor_device(enum sensor_kind kind, union sensor_value *value)
{
    switch (kind) {
    case SENSOR_AP3216C: return ap3216c_read_sample(&value->ap3216c);
    case SENSOR_ICM20608: return icm20608_read_sample(&value->icm20608);
    case SENSOR_DHT11: return dht11_read_sample(&value->dht11);
    case SENSOR_SR04: return sr04_read_sample(&value->sr04);
    case SENSOR_SR501: return sr501_read_sample(&value->sr501);
    default: errno = EINVAL; return -1;
    }
}

static int read_sensor(struct sensor_sampler *sampler, enum sensor_kind kind,
                        union sensor_value *value)
{
    if (kind != SENSOR_DHT11 && kind != SENSOR_SR04)
        return read_sensor_device(kind, value);
    /* DHT11 驱动会短暂关中断，不能与依赖回波 IRQ 时间戳的 SR04 重叠。 */
    pthread_mutex_lock(&sampler->timing_mutex);
    pthread_mutex_lock(&sampler->mutex);
    const int stopping = sampler->stopping;
    pthread_mutex_unlock(&sampler->mutex);
    int result = -1;
    if (stopping)
        errno = ECANCELED;
    else
        result = read_sensor_device(kind, value);
    const int error = errno;
    pthread_mutex_unlock(&sampler->timing_mutex);
    errno = error;
    return result;
}

static void *sample_sensor(void *opaque)
{
    const struct sampler_argument *argument = opaque;
    struct sensor_sampler *sampler = argument->sampler;
    const enum sensor_kind kind = argument->kind;

    for (;;) {
        pthread_mutex_lock(&sampler->mutex);
        const int stopping = sampler->stopping;
        pthread_mutex_unlock(&sampler->mutex);
        if (stopping)
            break;

        union sensor_value value;
        memset(&value, 0, sizeof(value));
        /* 每个设备只有一个采集线程；设备 I/O 期间不持有缓存锁。 */
        int error = read_sensor(sampler, kind, &value) < 0 ? (errno ? errno : EIO) : 0;
        struct timespec completed = {0, 0};
        const int clock_error = clock_gettime(CLOCK_MONOTONIC, &completed) < 0 ? errno : 0;
        if (clock_error != 0)
            error = clock_error;
        const struct timespec deadline = timespec_after_ms(
            completed, sampler->policies[kind].interval_ms);

        pthread_mutex_lock(&sampler->mutex);
        struct sensor_cache *cache = &sampler->caches[kind];
        cache->ready = 1;
        cache->error = error;
        if (error == 0) {
            cache->snapshot.value = value;
            cache->snapshot.completed_at = completed;
            ++cache->snapshot.sequence;
        }
        int wait_error = 0;
        while (!sampler->stopping && clock_error == 0 && wait_error == 0)
            wait_error = worker_condition_wait(&sampler->changed,
                                               &sampler->mutex, &deadline);
        if (wait_error != 0 && wait_error != ETIMEDOUT)
            cache->error = wait_error;
        const int finished = sampler->stopping || clock_error != 0 ||
            (wait_error != 0 && wait_error != ETIMEDOUT);
        pthread_mutex_unlock(&sampler->mutex);
        if (finished)
            break;
    }
    return NULL;
}

struct sensor_sampler *sensor_sampler_create(const struct sensor_policy *policies)
{
    if (policies == NULL)
        policies = default_policies;
    for (unsigned int i = 0; i < SENSOR_COUNT; ++i) {
        if (policies[i].interval_ms == 0 ||
            policies[i].max_age_ms < policies[i].interval_ms) {
            errno = EINVAL;
            return NULL;
        }
    }
    struct sensor_sampler *sampler = calloc(1, sizeof(*sampler));
    if (sampler == NULL)
        return NULL;
    int error = pthread_mutex_init(&sampler->mutex, NULL);
    if (error != 0)
        goto fail_memory;
    error = pthread_mutex_init(&sampler->timing_mutex, NULL);
    if (error != 0)
        goto fail_mutex;
    error = worker_condition_init(&sampler->changed);
    if (error != 0)
        goto fail_timing;
    memcpy(sampler->policies, policies, sizeof(sampler->policies));
    for (unsigned int i = 0; i < SENSOR_COUNT; ++i) {
        sampler->arguments[i].sampler = sampler;
        sampler->arguments[i].kind = (enum sensor_kind)i;
        error = worker_thread_create(&sampler->threads[i], sample_sensor,
                                      &sampler->arguments[i]);
        if (error != 0) {
            sensor_sampler_destroy(sampler);
            errno = error;
            return NULL;
        }
        ++sampler->started;
    }
    return sampler;

fail_timing:
    pthread_mutex_destroy(&sampler->timing_mutex);
fail_mutex:
    pthread_mutex_destroy(&sampler->mutex);
fail_memory:
    free(sampler);
    errno = error;
    return NULL;
}

int sensor_sampler_get(struct sensor_sampler *sampler, enum sensor_kind kind,
                       struct sensor_snapshot *snapshot)
{
    if (sampler == NULL || snapshot == NULL || kind < 0 || kind >= SENSOR_COUNT) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&sampler->mutex);
    const struct sensor_cache cache = sampler->caches[kind];
    const int stopping = sampler->stopping;
    pthread_mutex_unlock(&sampler->mutex);
    if (stopping || !cache.ready || cache.error != 0) {
        errno = stopping ? ECANCELED : (!cache.ready ? EAGAIN : cache.error);
        return -1;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    const int64_t age_ns = (int64_t)(now.tv_sec - cache.snapshot.completed_at.tv_sec)
        * 1000000000LL + now.tv_nsec - cache.snapshot.completed_at.tv_nsec;
    if (age_ns < 0 || age_ns > (int64_t)sampler->policies[kind].max_age_ms * 1000000) {
        errno = ETIMEDOUT;
        return -1;
    }
    *snapshot = cache.snapshot;
    return 0;
}

void sensor_sampler_request_stop(struct sensor_sampler *sampler)
{
    if (sampler == NULL)
        return;
    pthread_mutex_lock(&sampler->mutex);
    sampler->stopping = 1;
    pthread_cond_broadcast(&sampler->changed);
    pthread_mutex_unlock(&sampler->mutex);
}

void sensor_sampler_destroy(struct sensor_sampler *sampler)
{
    if (sampler == NULL)
        return;
    sensor_sampler_request_stop(sampler);
    for (unsigned int i = 0; i < sampler->started; ++i)
        pthread_join(sampler->threads[i], NULL);
    pthread_cond_destroy(&sampler->changed);
    pthread_mutex_destroy(&sampler->timing_mutex);
    pthread_mutex_destroy(&sampler->mutex);
    free(sampler);
}
