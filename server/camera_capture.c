#include "camera_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_COUNT 4
#define FRAME_WAIT_MS 3000

struct mapped_buffer {
    void *address;
    size_t length;
};

struct camera_stream {
    int fd;
    int streaming;
    unsigned int mapped_count;
    unsigned int width;
    unsigned int height;
    struct mapped_buffer buffers[BUFFER_COUNT];
};

static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int monotonic_ms(int64_t *milliseconds)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    *milliseconds = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return 0;
}

/* 非阻塞设备配合 poll 等待一帧；被信号打断时不重置总等待期限。 */
static int dequeue_frame(int fd, struct v4l2_buffer *buffer)
{
    int64_t deadline;
    if (monotonic_ms(&deadline) < 0)
        return -1;
    deadline += FRAME_WAIT_MS;

    for (;;) {
        int64_t now;
        if (monotonic_ms(&now) < 0)
            return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct pollfd descriptor = {0};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        int result = poll(&descriptor, 1, (int)(deadline - now));
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (!(descriptor.revents & POLLIN)) {
            errno = EIO;
            return -1;
        }

        if (ioctl(fd, VIDIOC_DQBUF, buffer) == 0)
            return 0;
        if (errno != EAGAIN && errno != EINTR)
            return -1;
    }
}

void camera_jpeg_release(struct camera_jpeg *image)
{
    if (image == NULL)
        return;
    free(image->data);
    memset(image, 0, sizeof(*image));
}

int camera_stream_close(struct camera_stream **stream)
{
    if (stream == NULL || *stream == NULL)
        return 0;

    struct camera_stream *camera = *stream;
    *stream = NULL;
    int saved_error = 0;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (camera->streaming && xioctl(camera->fd, VIDIOC_STREAMOFF, &type) < 0)
        saved_error = errno;
    for (unsigned int i = 0; i < camera->mapped_count; ++i) {
        if (munmap(camera->buffers[i].address, camera->buffers[i].length) < 0 &&
            saved_error == 0)
            saved_error = errno;
    }
    if (camera->fd >= 0 && close(camera->fd) < 0 && saved_error == 0)
        saved_error = errno;
    free(camera);
    if (saved_error != 0) {
        errno = saved_error;
        return -1;
    }
    return 0;
}

int camera_stream_open(const char *device, struct camera_stream **stream)
{
    if (device == NULL || device[0] == '\0' || stream == NULL || *stream != NULL) {
        errno = EINVAL;
        return -1;
    }

    int saved_error;
    struct camera_stream *camera = calloc(1, sizeof(*camera));
    if (camera == NULL)
        return -1;
    camera->fd = -1;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_capability capability;
    struct v4l2_format format;
    struct v4l2_requestbuffers request;
    struct v4l2_buffer buffer;
    /* 兼容 GCC 4.9.4 对含数组/联合体的聚合初始化诊断。 */
    memset(&capability, 0, sizeof(capability));
    memset(&format, 0, sizeof(format));
    memset(&request, 0, sizeof(request));

    camera->fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (camera->fd < 0)
        goto cleanup;

    if (xioctl(camera->fd, VIDIOC_QUERYCAP, &capability) < 0)
        goto cleanup;
    uint32_t caps = capability.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS)
        caps = capability.device_caps;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        errno = ENOTSUP;
        goto cleanup;
    }

    format.type = type;
    format.fmt.pix.width = 640;
    format.fmt.pix.height = 480;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(camera->fd, VIDIOC_S_FMT, &format) < 0)
        goto cleanup;
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        errno = ENOTSUP;
        goto cleanup;
    }

    request.count = BUFFER_COUNT;
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera->fd, VIDIOC_REQBUFS, &request) < 0)
        goto cleanup;
    if (request.count == 0 || request.count > BUFFER_COUNT) {
        errno = ENOBUFS;
        goto cleanup;
    }

    for (unsigned int i = 0; i < request.count; ++i) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(camera->fd, VIDIOC_QUERYBUF, &buffer) < 0)
            goto cleanup;

        camera->buffers[i].length = buffer.length;
        camera->buffers[i].address = mmap(NULL, buffer.length,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         camera->fd, buffer.m.offset);
        if (camera->buffers[i].address == MAP_FAILED)
            goto cleanup;
        ++camera->mapped_count;
    }

    for (unsigned int i = 0; i < request.count; ++i) {
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (xioctl(camera->fd, VIDIOC_QBUF, &buffer) < 0)
            goto cleanup;
    }

    if (xioctl(camera->fd, VIDIOC_STREAMON, &type) < 0)
        goto cleanup;
    camera->streaming = 1;
    camera->width = format.fmt.pix.width;
    camera->height = format.fmt.pix.height;
    *stream = camera;
    return 0;

cleanup:
    saved_error = errno;
    camera_stream_close(&camera);
    errno = saved_error;
    return -1;
}

int camera_stream_read(struct camera_stream *camera, struct camera_jpeg *image)
{
    if (camera == NULL || !camera->streaming || image == NULL || image->data != NULL) {
        errno = EINVAL;
        return -1;
    }

    int result = -1;
    int saved_error;
    struct camera_jpeg next = {0};
    struct v4l2_buffer frames[BUFFER_COUNT];
    unsigned int held = 0;
    memset(frames, 0, sizeof(frames));
    for (unsigned int i = 0; i < BUFFER_COUNT; ++i) {
        frames[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        frames[i].memory = V4L2_MEMORY_MMAP;
    }

    if (dequeue_frame(camera->fd, &frames[0]) < 0)
        return -1;
    held = 1;
    // 客户端处理较慢时取当前队列中较新的一帧，最多出队4块，避免积压旧画面。
    while (held < camera->mapped_count) {
        if (xioctl(camera->fd, VIDIOC_DQBUF, &frames[held]) < 0) {
            if (errno == EAGAIN)
                break;
            goto cleanup;
        }
        ++held;
    }
    struct v4l2_buffer *buffer = &frames[held - 1];
    if (buffer->index >= camera->mapped_count ||
        (buffer->flags & V4L2_BUF_FLAG_ERROR) || buffer->bytesused < 2 ||
        buffer->bytesused > CAMERA_MAX_JPEG_BYTES ||
        buffer->bytesused > camera->buffers[buffer->index].length) {
        errno = EIO;
        goto cleanup;
    }
    const unsigned char *data = camera->buffers[buffer->index].address;
    if (data[0] != 0xff || data[1] != 0xd8) {
        errno = EIO;
        goto cleanup;
    }
    next.data = malloc(buffer->bytesused);
    if (next.data == NULL)
        goto cleanup;
    memcpy(next.data, data, buffer->bytesused);
    next.size = buffer->bytesused;
    next.width = camera->width;
    next.height = camera->height;
    result = 0;

cleanup:
    saved_error = result < 0 ? errno : 0;
    // JPEG 已复制，所有出队缓冲都归还；下一次读取继续复用同一个流。
    for (unsigned int i = 0; i < held; ++i) {
        if (frames[i].index >= camera->mapped_count) {
            if (saved_error == 0)
                saved_error = EIO;
            continue;
        }
        if (xioctl(camera->fd, VIDIOC_QBUF, &frames[i]) < 0 && saved_error == 0)
            saved_error = errno;
    }
    if (saved_error != 0) {
        camera_jpeg_release(&next);
        errno = saved_error;
        return -1;
    }
    *image = next;
    return 0;
}

int camera_capture_jpeg(const char *device, struct camera_jpeg *image)
{
    if (image == NULL || image->data != NULL) {
        errno = EINVAL;
        return -1;
    }
    struct camera_stream *stream = NULL;
    struct camera_jpeg next = {0};
    if (camera_stream_open(device, &stream) < 0)
        return -1;
    int result = camera_stream_read(stream, &next);
    int saved_error = result < 0 ? errno : 0;
    if (camera_stream_close(&stream) < 0 && saved_error == 0)
        saved_error = errno;
    if (saved_error != 0) {
        camera_jpeg_release(&next);
        errno = saved_error;
        return -1;
    }
    *image = next;
    return 0;
}
