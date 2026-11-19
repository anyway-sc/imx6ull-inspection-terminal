#include "camera_worker.h"
#include "thread_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct camera_worker {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    int stopping;
    char *device;
    void (*notify)(void *);
    void *context;
    struct camera_request jobs[CAMERA_JOB_CAPACITY];
    unsigned int job_head, job_count;
    struct camera_reply replies[CAMERA_REPLY_CAPACITY];
    unsigned int reply_head, reply_count;
};

void camera_reply_release(struct camera_reply *reply)
{
    if (reply != NULL) {
        camera_jpeg_release(&reply->image);
        memset(reply, 0, sizeof(*reply));
    }
}

static struct camera_reply execute_request(const char *device,
                                            struct camera_stream **stream,
                                            struct camera_request request)
{
    struct camera_reply reply;
    memset(&reply, 0, sizeof(reply));
    reply.id = request.id;
    reply.status = "OK";
    if (request.kind == CAMERA_PREVIEW_START) {
        reply.detail = "PREVIEW_STARTED";
        if (*stream == NULL && camera_stream_open(device, stream) < 0) {
            reply.status = "ERR";
            reply.detail = "CAMERA_START";
        }
    } else if (request.kind == CAMERA_PREVIEW_STOP) {
        reply.detail = "PREVIEW_STOPPED";
        if (camera_stream_close(stream) < 0) {
            reply.status = "ERR";
            reply.detail = "CAMERA_STOP";
        }
    } else if (request.kind == CAMERA_PREVIEW_FRAME && *stream == NULL) {
        reply.status = "ERR";
        reply.detail = "PREVIEW_NOT_STARTED";
    } else if (request.kind == CAMERA_CAPTURE && *stream != NULL) {
        reply.status = "ERR";
        reply.detail = "CAMERA_BUSY";
    } else {
        const int result = request.kind == CAMERA_CAPTURE
            ? camera_capture_jpeg(device, &reply.image)
            : camera_stream_read(*stream, &reply.image);
        if (result < 0 || reply.image.data == NULL || reply.image.size < 2 ||
            reply.image.size > CAMERA_MAX_JPEG_BYTES) {
            camera_jpeg_release(&reply.image);
            camera_stream_close(stream);
            reply.status = "ERR";
            reply.detail = request.kind == CAMERA_CAPTURE ? "CAMERA_CAPTURE" : "CAMERA_FRAME";
        }
    }
    return reply;
}

static void *run_camera(void *opaque)
{
    struct camera_worker *worker = opaque;
    /* 流和 V4L2 映射始终归该线程所有；其他线程不会关闭正在使用的流。 */
    struct camera_stream *stream = NULL;
    for (;;) {
        pthread_mutex_lock(&worker->mutex);
        while (!worker->stopping && (worker->job_count == 0 ||
                                    worker->reply_count == CAMERA_REPLY_CAPACITY))
            pthread_cond_wait(&worker->changed, &worker->mutex);
        if (worker->stopping) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }
        const struct camera_request request = worker->jobs[worker->job_head];
        worker->job_head = (worker->job_head + 1) % CAMERA_JOB_CAPACITY;
        --worker->job_count;
        pthread_mutex_unlock(&worker->mutex);

        struct camera_reply reply = execute_request(worker->device, &stream, request);

        pthread_mutex_lock(&worker->mutex);
        if (worker->stopping) {
            pthread_mutex_unlock(&worker->mutex);
            camera_reply_release(&reply);
            break;
        }
        const unsigned int tail = (worker->reply_head + worker->reply_count)
            % CAMERA_REPLY_CAPACITY;
        worker->replies[tail] = reply;
        ++worker->reply_count;
        pthread_mutex_unlock(&worker->mutex);
        if (worker->notify != NULL)
            worker->notify(worker->context);
    }
    camera_stream_close(&stream);
    return NULL;
}

struct camera_worker *camera_worker_create(const char *device,
                                           void (*notify)(void *), void *context)
{
    if (device == NULL || device[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    struct camera_worker *worker = calloc(1, sizeof(*worker));
    if (worker == NULL)
        return NULL;
    worker->device = strdup(device);
    if (worker->device == NULL) {
        free(worker);
        return NULL;
    }
    worker->notify = notify;
    worker->context = context;
    int error = pthread_mutex_init(&worker->mutex, NULL);
    if (error != 0)
        goto fail_memory;
    error = pthread_cond_init(&worker->changed, NULL);
    if (error != 0)
        goto fail_mutex;
    error = worker_thread_create(&worker->thread, run_camera, worker);
    if (error != 0) {
        pthread_cond_destroy(&worker->changed);
        goto fail_mutex;
    }
    return worker;

fail_mutex:
    pthread_mutex_destroy(&worker->mutex);
fail_memory:
    free(worker->device);
    free(worker);
    errno = error;
    return NULL;
}

int camera_worker_submit(struct camera_worker *worker, struct camera_request request)
{
    if (worker == NULL || request.id == 0 || request.kind < CAMERA_CAPTURE ||
        request.kind > CAMERA_PREVIEW_STOP) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&worker->mutex);
    int error = 0;
    if (worker->stopping)
        error = ECANCELED;
    else if (worker->job_count == CAMERA_JOB_CAPACITY)
        error = EAGAIN;
    else {
        const unsigned int tail = (worker->job_head + worker->job_count) % CAMERA_JOB_CAPACITY;
        worker->jobs[tail] = request;
        ++worker->job_count;
        pthread_cond_signal(&worker->changed);
    }
    pthread_mutex_unlock(&worker->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int camera_worker_take(struct camera_worker *worker, struct camera_reply *reply)
{
    if (worker == NULL || reply == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&worker->mutex);
    const int available = worker->reply_count > 0;
    if (available) {
        *reply = worker->replies[worker->reply_head];
        memset(&worker->replies[worker->reply_head], 0, sizeof(*reply));
        worker->reply_head = (worker->reply_head + 1) % CAMERA_REPLY_CAPACITY;
        --worker->reply_count;
        pthread_cond_signal(&worker->changed);
    }
    pthread_mutex_unlock(&worker->mutex);
    return available;
}

void camera_worker_destroy(struct camera_worker *worker)
{
    if (worker == NULL)
        return;
    pthread_mutex_lock(&worker->mutex);
    worker->stopping = 1;
    worker->job_count = 0;
    pthread_cond_broadcast(&worker->changed);
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);
    for (unsigned int i = 0; i < CAMERA_REPLY_CAPACITY; ++i)
        camera_reply_release(&worker->replies[i]);
    pthread_cond_destroy(&worker->changed);
    pthread_mutex_destroy(&worker->mutex);
    free(worker->device);
    free(worker);
}
