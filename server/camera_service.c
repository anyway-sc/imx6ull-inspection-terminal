#define _GNU_SOURCE
#include "camera_worker.h"
#include "ipc_protocol.h"
#include "process_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void stop_service(int number)
{
    (void)number;
    stopping = 1;
}

/* 字节流必须处理部分收发；空闲等待可被 SIGTERM 或父进程关闭打断。 */
static int transfer_exact(int fd, void *buffer, size_t length, int sending)
{
    unsigned char *bytes = buffer;
    size_t offset = 0;
    int64_t deadline = process_monotonic_ms() + 10000;
    while (offset < length && !stopping) {
        const ssize_t n = sending
            ? send(fd, bytes + offset, length - offset, MSG_NOSIGNAL)
            : recv(fd, bytes + offset, length - offset, 0);
        if (n > 0) {
            if (offset == 0 && !sending) deadline = process_monotonic_ms() + 10000;
            offset += (size_t)n;
            continue;
        }
        if (n == 0) { errno = EPIPE; return -1; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if ((sending || offset != 0) && process_monotonic_ms() >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd descriptor = {fd, sending ? POLLOUT : POLLIN, 0};
        const int result = poll(&descriptor, 1, 100);
        if (result < 0 && errno != EINTR) return -1;
        if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
            !(descriptor.revents & (sending ? POLLOUT : POLLIN))) {
            errno = EPIPE;
            return -1;
        }
    }
    if (offset != length) { errno = ECANCELED; return -1; }
    return 0;
}

static enum ipc_result execute_request(const char *device, struct camera_stream **stream,
                                       enum camera_request_kind kind, struct camera_jpeg *image)
{
    if (kind == CAMERA_PREVIEW_START)
        return *stream == NULL && camera_stream_open(device, stream) < 0
            ? IPC_CAMERA_START : IPC_PREVIEW_STARTED;
    if (kind == CAMERA_PREVIEW_STOP)
        return camera_stream_close(stream) < 0 ? IPC_CAMERA_STOP : IPC_PREVIEW_STOPPED;
    if (kind == CAMERA_PREVIEW_FRAME && *stream == NULL)
        return IPC_PREVIEW_NOT_STARTED;
    if (kind == CAMERA_CAPTURE && *stream != NULL)
        return IPC_CAMERA_BUSY;
    const int result = kind == CAMERA_CAPTURE
        ? camera_capture_jpeg(device, image) : camera_stream_read(*stream, image);
    if (result < 0 || image->data == NULL || image->size < 4 ||
        image->size > CAMERA_MAX_JPEG_BYTES) {
        camera_jpeg_release(image);
        camera_stream_close(stream);
        return kind == CAMERA_CAPTURE ? IPC_CAMERA_CAPTURE : IPC_CAMERA_FRAME;
    }
    return IPC_JPEG;
}

static int parse_number(const char *text, uint32_t *number)
{
    if (text[0] < '0' || text[0] > '9') return -1;
    char *end;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > UINT32_MAX) return -1;
    *number = (uint32_t)value;
    return 0;
}

int main(int argc, char *argv[])
{
    uint32_t fd_number, parent, session, generation;
    if (argc != 6 || parse_number(argv[1], &fd_number) < 0 || fd_number != 3 ||
        parse_number(argv[2], &parent) < 0 || parent > INT_MAX ||
        parse_number(argv[3], &session) < 0 || parse_number(argv[4], &generation) < 0) {
        fprintf(stderr, "相机服务需由主进程创建并传入 IPC 通道。\n");
        return EXIT_FAILURE;
    }
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != (pid_t)parent)
        return EXIT_FAILURE;
    const int fd = (int)fd_number;
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        return EXIT_FAILURE;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_service;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0)
        return EXIT_FAILURE;

    unsigned char wire[IPC_HEADER_BYTES];
    struct ipc_header header = {IPC_READY, session, generation, 0, 0, 0, 0, 0};
    ipc_header_encode(wire, &header);
    if (transfer_exact(fd, wire, sizeof(wire), 1) < 0) { close(fd); return EXIT_FAILURE; }
    struct camera_stream *stream = NULL;
    int exit_status = EXIT_SUCCESS;
    while (!stopping) {
        if (transfer_exact(fd, wire, sizeof(wire), 0) < 0) break;
        if (ipc_header_decode(wire, &header) < 0 || header.type != IPC_REQUEST ||
            header.session != session || header.generation != generation || header.id == 0 ||
            header.code > CAMERA_PREVIEW_STOP || header.length != 0 ||
            header.width != 0 || header.height != 0) {
            exit_status = EXIT_FAILURE;
            break;
        }
        struct camera_jpeg image = {0};
        header.type = IPC_REPLY;
        header.code = execute_request(argv[5], &stream, (enum camera_request_kind)header.code, &image);
        header.length = (uint32_t)image.size;
        header.width = image.width;
        header.height = image.height;
        ipc_header_encode(wire, &header);
        int result = transfer_exact(fd, wire, sizeof(wire), 1);
        if (result == 0 && image.data != NULL)
            result = transfer_exact(fd, image.data, image.size, 1);
        camera_jpeg_release(&image);
        if (result < 0) break;
    }
    camera_stream_close(&stream);
    close(fd);
    return exit_status;
}
