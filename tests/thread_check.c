/* 检查并发和生命周期，故障通过链接器包装注入，不给正式服务加入测试开关。 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "fake_devices.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned int create_calls, fail_create_at;
static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int live_threads;
struct tracked_start { void *(*entry)(void *); void *argument; };

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

static void *tracked_entry(void *opaque)
{
    const struct tracked_start start = *(struct tracked_start *)opaque;
    free(opaque);
    void *result = start.entry(start.argument);
    pthread_mutex_lock(&thread_mutex);
    --live_threads;
    pthread_mutex_unlock(&thread_mutex);
    return result;
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument)
{
    if (++create_calls == fail_create_at) return EAGAIN;
    struct tracked_start *start = malloc(sizeof(*start));
    assert(start != NULL);
    start->entry = entry; start->argument = argument;
    pthread_mutex_lock(&thread_mutex);
    ++live_threads;
    pthread_mutex_unlock(&thread_mutex);
    const int result = __real_pthread_create(thread, attributes, tracked_entry, start);
    if (result != 0) {
        free(start);
        pthread_mutex_lock(&thread_mutex);
        --live_threads;
        pthread_mutex_unlock(&thread_mutex);
    }
    return result;
}

static int64_t milliseconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void pause_ms(unsigned int milliseconds)
{
    const struct timespec time = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&time, NULL);
}

#define WAIT_FOR(condition) do { \
    const int64_t limit = milliseconds() + 3000; \
    while (!(condition) && milliseconds() < limit) pause_ms(1); \
    assert(condition); \
} while (0)

static void check_resources(void)
{
    pthread_mutex_lock(&thread_mutex);
    assert(live_threads == 0);
    pthread_mutex_unlock(&thread_mutex);
    assert(fake_sensor_active() == 0);
    assert(fake_timing_overlap_error() == 0);
    assert(fake_camera_active() == 0);
    assert(fake_camera_images() == 0);
    assert(fake_camera_open_streams() == 0);
    assert(fake_camera_owner_error() == 0);
}

static void check_sampling(void)
{
    fake_reset();
    struct sensor_policy policies[SENSOR_COUNT];
    for (unsigned int i = 0; i < SENSOR_COUNT; ++i) {
        policies[i].interval_ms = 10; policies[i].max_age_ms = 100;
        fake_sensor_block((enum sensor_kind)i, 1);
    }
    struct sensor_sampler *sampler = sensor_sampler_create(policies);
    if (sampler == NULL)
        fprintf(stderr, "sensor_sampler_create: %d (%s)\n", errno, strerror(errno));
    assert(sampler != NULL);
    /* 五个线程中两个时序设备互斥，其余四个设备读取可同时进入。 */
    WAIT_FOR(fake_sensor_active() == SENSOR_COUNT - 1);
    assert(fake_sensor_calls(SENSOR_DHT11) + fake_sensor_calls(SENSOR_SR04) == 1);
    struct sensor_snapshot snapshot, original;
    memset(&snapshot, 0x55, sizeof(snapshot));
    original = snapshot;
    assert(sensor_sampler_get(sampler, SENSOR_SR04, &snapshot) == -1 && errno == EAGAIN);
    assert(memcmp(&snapshot, &original, sizeof(snapshot)) == 0);
    for (unsigned int i = 0; i < SENSOR_COUNT; ++i)
        if (i != SENSOR_SR04) fake_sensor_block((enum sensor_kind)i, 0);
    WAIT_FOR(fake_sensor_calls(SENSOR_SR04) > 0);
    WAIT_FOR(sensor_sampler_get(sampler, SENSOR_AP3216C, &snapshot) == 0);
    assert(fake_sensor_active() == 1); /* SR04 仍阻塞，AP 已交付。 */
    for (unsigned int i = 0; i < 50000; ++i) {
        assert(sensor_sampler_get(sampler, SENSOR_AP3216C, &snapshot) == 0);
        assert(snapshot.value.ap3216c.als == snapshot.value.ap3216c.ir * 2);
        assert(snapshot.value.ap3216c.ps == snapshot.value.ap3216c.ir * 3);
    }
    fake_sensor_block(SENSOR_SR04, 0);
    WAIT_FOR(sensor_sampler_get(sampler, SENSOR_SR04, &snapshot) == 0);
    WAIT_FOR(sensor_sampler_get(sampler, SENSOR_SR501, &snapshot) == 0);
    fake_sensor_fail(SENSOR_SR501, 1);
    WAIT_FOR(sensor_sampler_get(sampler, SENSOR_SR501, &snapshot) < 0 && errno == EIO);
    fake_sensor_fail(SENSOR_SR501, 0);
    WAIT_FOR(sensor_sampler_get(sampler, SENSOR_SR501, &snapshot) == 0);

    fake_sensor_block(SENSOR_AP3216C, 1);
    const unsigned int previous = fake_sensor_calls(SENSOR_AP3216C);
    WAIT_FOR(fake_sensor_calls(SENSOR_AP3216C) > previous);
    pause_ms(130);
    assert(sensor_sampler_get(sampler, SENSOR_AP3216C, &snapshot) == -1 && errno == ETIMEDOUT);
    fake_sensor_block(SENSOR_AP3216C, 0);
    sensor_sampler_request_stop(sampler);
    assert(sensor_sampler_get(sampler, SENSOR_SR04, &snapshot) == -1 && errno == ECANCELED);
    sensor_sampler_destroy(sampler);
    check_resources();
    puts("通过：独立采集并发、DHT/SR04 时序互斥、慢设备隔离、完整快照、失败/过期及恢复");
}

static void check_idle_shutdown(void)
{
    fake_reset();
    struct sensor_sampler *sampler = sensor_sampler_create(NULL);
    assert(sampler != NULL);
    struct sensor_snapshot snapshot;
    WAIT_FOR(sensor_sampler_get(sampler, SENSOR_DHT11, &snapshot) == 0);
    const int64_t begin = milliseconds();
    sensor_sampler_destroy(sampler);
    assert(milliseconds() - begin < 1000); /* 不必等待 DHT 的 3s 周期。 */
    check_resources();
    puts("通过：条件变量唤醒周期等待，退出后全部线程已回收");
}

static void check_create_failure(void)
{
    for (unsigned int fail = 1; fail <= SENSOR_COUNT; ++fail) {
        fake_reset(); create_calls = 0; fail_create_at = fail;
        assert(sensor_sampler_create(NULL) == NULL && errno == EAGAIN);
        check_resources();
    }
    fail_create_at = 0;
    puts("通过：逐个注入线程创建失败，回收已启动线程和初始化资源");
}

int main(void)
{
    check_sampling();
    check_idle_shutdown();
    check_create_failure();
    return 0;
}
