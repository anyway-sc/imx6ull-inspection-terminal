/* 检查并发和生命周期，故障通过链接器包装注入，不给正式服务加入测试开关。 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "camera_worker.h"
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

static struct camera_reply take_reply(struct camera_worker *worker)
{
    struct camera_reply reply;
    int available;
    const int64_t deadline = milliseconds() + 3000;
    do {
        available = camera_worker_take(worker, &reply);
        if (!available) pause_ms(1);
    } while (!available && milliseconds() < deadline);
    assert(available == 1);
    return reply;
}

static void submit(struct camera_worker *worker, enum camera_request_kind kind, unsigned int id)
{
    const struct camera_request request = {kind, id};
    assert(camera_worker_submit(worker, request) == 0);
}

static void start_preview(struct camera_worker *worker)
{
    submit(worker, CAMERA_PREVIEW_START, 1);
    struct camera_reply reply = take_reply(worker);
    assert(strcmp(reply.status, "OK") == 0);
    assert(strcmp(reply.detail, "PREVIEW_STARTED") == 0);
    camera_reply_release(&reply);
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
    fake_reset(); create_calls = 0; fail_create_at = 1;
    assert(camera_worker_create("fake", NULL, NULL) == NULL && errno == EAGAIN);
    check_resources();
    fail_create_at = 0;
    puts("通过：逐个注入线程创建失败，回收已启动线程和初始化资源");
}

static void check_camera_queue(void)
{
    fake_reset();
    struct camera_worker *worker = camera_worker_create("fake", NULL, NULL);
    assert(worker != NULL);
    start_preview(worker);
    fake_camera_block(1);
    submit(worker, CAMERA_PREVIEW_FRAME, 2);
    WAIT_FOR(fake_camera_active() == 1);
    for (unsigned int i = 0; i < CAMERA_JOB_CAPACITY; ++i)
        submit(worker, CAMERA_PREVIEW_FRAME, 3 + i);
    const struct camera_request extra = {CAMERA_PREVIEW_FRAME, 99};
    assert(camera_worker_submit(worker, extra) == -1 && errno == EAGAIN);
    fake_camera_block(0);
    for (unsigned int i = 0; i <= CAMERA_JOB_CAPACITY; ++i) {
        struct camera_reply reply = take_reply(worker);
        assert(reply.id == i + 2 && reply.image.size == 32);
        assert(reply.image.data[0] == 0xff && reply.image.data[2] == '\n');
        camera_reply_release(&reply);
    }
    fake_camera_fail(1);
    submit(worker, CAMERA_PREVIEW_FRAME, 100);
    struct camera_reply reply = take_reply(worker);
    assert(strcmp(reply.detail, "CAMERA_FRAME") == 0);
    camera_reply_release(&reply);
    assert(fake_camera_open_streams() == 0);
    submit(worker, CAMERA_PREVIEW_FRAME, 101);
    reply = take_reply(worker);
    assert(strcmp(reply.detail, "PREVIEW_NOT_STARTED") == 0);
    camera_reply_release(&reply);
    fake_camera_fail(0);
    start_preview(worker);
    submit(worker, CAMERA_CAPTURE, 102);
    reply = take_reply(worker);
    assert(strcmp(reply.detail, "CAMERA_BUSY") == 0);
    camera_reply_release(&reply);
    submit(worker, CAMERA_PREVIEW_STOP, 103);
    reply = take_reply(worker);
    assert(strcmp(reply.detail, "PREVIEW_STOPPED") == 0);
    camera_reply_release(&reply);
    submit(worker, CAMERA_CAPTURE, 104);
    reply = take_reply(worker);
    assert(reply.image.data != NULL);
    camera_reply_release(&reply);
    camera_worker_destroy(worker);
    check_resources();
    puts("通过：相机独占、有界队列、FIFO、JPEG 所有权、错误清理和重启");
}

static void check_full_reply_shutdown(void)
{
    fake_reset();
    struct camera_worker *worker = camera_worker_create("fake", NULL, NULL);
    assert(worker != NULL);
    start_preview(worker);
    for (unsigned int i = 0; i < CAMERA_JOB_CAPACITY; ++i)
        submit(worker, CAMERA_PREVIEW_FRAME, 2 + i);
    WAIT_FOR(fake_camera_images() == CAMERA_REPLY_CAPACITY);
    pause_ms(20);
    assert(fake_camera_images() == CAMERA_REPLY_CAPACITY);
    camera_worker_destroy(worker);
    check_resources();
    puts("通过：回复队列满时退出不死锁，释放未发送 JPEG、丢弃未执行请求");
}

static void *release_camera(void *unused)
{
    (void)unused;
    pause_ms(30);
    fake_camera_block(0);
    return NULL;
}

static void check_inflight_shutdown(void)
{
    fake_reset();
    struct camera_worker *worker = camera_worker_create("fake", NULL, NULL);
    assert(worker != NULL);
    start_preview(worker);
    fake_camera_block(1);
    submit(worker, CAMERA_PREVIEW_FRAME, 2);
    WAIT_FOR(fake_camera_active() == 1);
    pthread_t release;
    assert(pthread_create(&release, NULL, release_camera, NULL) == 0);
    camera_worker_destroy(worker);
    pthread_join(release, NULL);
    check_resources();
    puts("通过：读取进行中退出，等待设备返回后统一回收，不跨线程关闭设备");
}

int main(void)
{
    check_sampling();
    check_idle_shutdown();
    check_create_failure();
    check_camera_queue();
    check_full_reply_shutdown();
    check_inflight_shutdown();
    return 0;
}
