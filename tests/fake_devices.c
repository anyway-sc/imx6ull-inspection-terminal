/* 只链接到测试程序，替代真实设备；所有测试状态同样受互斥锁保护。 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "fake_devices.h"
#include "camera_capture.h"
#include "sr501.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned int calls[SENSOR_COUNT], active_sensors;
static int blocked[SENSOR_COUNT], failures[SENSOR_COUNT];
static unsigned int timing_active;
static int timing_overlap_error;
static unsigned int active_camera, images, streams;
static int camera_blocked, camera_failure, owner_error;

struct camera_stream { pthread_t owner; };

void fake_reset(void)
{
    pthread_mutex_lock(&mutex);
    assert(active_sensors == 0 && active_camera == 0 && images == 0 && streams == 0);
    memset(calls, 0, sizeof(calls));
    memset(blocked, 0, sizeof(blocked));
    memset(failures, 0, sizeof(failures));
    assert(timing_active == 0);
    timing_overlap_error = 0;
    camera_blocked = camera_failure = owner_error = 0;
    pthread_mutex_unlock(&mutex);
}

void fake_sensor_block(enum sensor_kind kind, int block)
{
    pthread_mutex_lock(&mutex);
    blocked[kind] = block;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
}

void fake_sensor_fail(enum sensor_kind kind, int fail)
{
    pthread_mutex_lock(&mutex);
    failures[kind] = fail;
    pthread_mutex_unlock(&mutex);
}

unsigned int fake_sensor_calls(enum sensor_kind kind)
{
    pthread_mutex_lock(&mutex);
    const unsigned int value = calls[kind];
    pthread_mutex_unlock(&mutex);
    return value;
}

unsigned int fake_sensor_active(void)
{
    pthread_mutex_lock(&mutex);
    const unsigned int value = active_sensors;
    pthread_mutex_unlock(&mutex);
    return value;
}

static int begin_sample(enum sensor_kind kind)
{
    pthread_mutex_lock(&mutex);
    const unsigned int sequence = ++calls[kind];
    ++active_sensors;
    const int timing_sensitive = kind == SENSOR_DHT11 || kind == SENSOR_SR04;
    if (timing_sensitive && ++timing_active > 1)
        timing_overlap_error = 1;
    while (blocked[kind])
        pthread_cond_wait(&changed, &mutex);
    const int failure = failures[kind];
    --active_sensors;
    if (timing_sensitive) --timing_active;
    pthread_mutex_unlock(&mutex);
    if (failure) {
        errno = EIO;
        return -1;
    }
    return (int)sequence;
}

int fake_timing_overlap_error(void)
{
    pthread_mutex_lock(&mutex);
    const int value = timing_overlap_error;
    pthread_mutex_unlock(&mutex);
    return value;
}

int ap3216c_read_sample(struct ap3216c_sample *sample)
{
    const int n = begin_sample(SENSOR_AP3216C);
    if (n < 0) return -1;
    sample->ir = n; sample->als = n * 2; sample->ps = n * 3;
    return 0;
}

int icm20608_read_sample(struct icm20608_sample *sample)
{
    const int n = begin_sample(SENSOR_ICM20608);
    if (n < 0) return -1;
    const struct icm20608_sample value = {n, -n, n * 2, -n * 2, n * 3, -n * 3, 25};
    *sample = value;
    return 0;
}

int dht11_read_sample(struct dht11_sample *sample)
{
    if (begin_sample(SENSOR_DHT11) < 0) return -1;
    sample->humidity_tenths = 600; sample->temperature_tenths = 260;
    return 0;
}

int sr04_read_sample(struct sr04_sample *sample)
{
    if (begin_sample(SENSOR_SR04) < 0) return -1;
    sample->distance_mm = 250; sample->pulse_us = 1458;
    return 0;
}

int sr501_read_sample(unsigned int *level)
{
    if (begin_sample(SENSOR_SR501) < 0) return -1;
    *level = 1;
    return 0;
}

void fake_camera_block(int block)
{
    pthread_mutex_lock(&mutex);
    camera_blocked = block;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
}

void fake_camera_fail(int fail)
{
    pthread_mutex_lock(&mutex);
    camera_failure = fail;
    pthread_mutex_unlock(&mutex);
}

unsigned int fake_camera_active(void)
{
    pthread_mutex_lock(&mutex);
    const unsigned int value = active_camera;
    pthread_mutex_unlock(&mutex);
    return value;
}

unsigned int fake_camera_images(void)
{
    pthread_mutex_lock(&mutex);
    const unsigned int value = images;
    pthread_mutex_unlock(&mutex);
    return value;
}

unsigned int fake_camera_open_streams(void)
{
    pthread_mutex_lock(&mutex);
    const unsigned int value = streams;
    pthread_mutex_unlock(&mutex);
    return value;
}

int fake_camera_owner_error(void)
{
    pthread_mutex_lock(&mutex);
    const int value = owner_error;
    pthread_mutex_unlock(&mutex);
    return value;
}

int camera_stream_open(const char *device, struct camera_stream **stream)
{
    (void)device;
    assert(*stream == NULL);
    *stream = malloc(sizeof(**stream));
    if (*stream == NULL) return -1;
    (*stream)->owner = pthread_self();
    pthread_mutex_lock(&mutex);
    ++streams;
    pthread_mutex_unlock(&mutex);
    return 0;
}

int camera_stream_read(struct camera_stream *stream, struct camera_jpeg *image)
{
    assert(stream != NULL && image->data == NULL);
    pthread_mutex_lock(&mutex);
    if (!pthread_equal(stream->owner, pthread_self())) owner_error = 1;
    ++active_camera;
    while (camera_blocked)
        pthread_cond_wait(&changed, &mutex);
    const int failure = camera_failure;
    pthread_mutex_unlock(&mutex);
    const char *delay_text = getenv("FAKE_CAMERA_DELAY_MS");
    if (delay_text != NULL) {
        const unsigned int delay = (unsigned int)strtoul(delay_text, NULL, 10);
        const struct timespec wait = {delay / 1000, (long)(delay % 1000) * 1000000L};
        nanosleep(&wait, NULL);
    }
    pthread_mutex_lock(&mutex);
    --active_camera;
    pthread_mutex_unlock(&mutex);
    if (failure) { errno = EIO; return -1; }
    size_t size = 32;
    const char *size_text = getenv("FAKE_JPEG_BYTES");
    if (size_text != NULL) size = (size_t)strtoul(size_text, NULL, 10);
    assert(size >= 4 && size <= CAMERA_MAX_JPEG_BYTES);
    image->data = malloc(size);
    if (image->data == NULL) return -1;
    memset(image->data, 0x5a, size);
    image->data[0] = 0xff; image->data[1] = 0xd8;
    image->data[2] = '\n'; image->data[3] = '\0';
    image->data[size - 2] = 0xff; image->data[size - 1] = 0xd9;
    image->size = size; image->width = 640; image->height = 480;
    pthread_mutex_lock(&mutex);
    ++images;
    pthread_mutex_unlock(&mutex);
    return 0;
}

void camera_jpeg_release(struct camera_jpeg *image)
{
    if (image == NULL) return;
    if (image->data != NULL) {
        free(image->data);
        pthread_mutex_lock(&mutex);
        assert(images > 0);
        --images;
        pthread_mutex_unlock(&mutex);
    }
    memset(image, 0, sizeof(*image));
}

int camera_stream_close(struct camera_stream **stream)
{
    if (stream == NULL || *stream == NULL) return 0;
    pthread_mutex_lock(&mutex);
    if (!pthread_equal((*stream)->owner, pthread_self())) owner_error = 1;
    assert(streams > 0);
    --streams;
    pthread_mutex_unlock(&mutex);
    free(*stream); *stream = NULL;
    return 0;
}

int camera_capture_jpeg(const char *device, struct camera_jpeg *image)
{
    struct camera_stream *stream = NULL;
    if (camera_stream_open(device, &stream) < 0) return -1;
    const int result = camera_stream_read(stream, image);
    camera_stream_close(&stream);
    return result;
}
