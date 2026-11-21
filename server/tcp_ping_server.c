#define _GNU_SOURCE
#include "camera_worker.h"
#include "process_manager.h"
#include "sensor_sampler.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 9000
#define LINE_CAPACITY 128
#define SEND_TIMEOUT_MS 5000

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t signal_pipe = -1;

/* 信号处理器只设置标志并唤醒 poll，不使用锁、free 或 pthread_join。 */
static void request_shutdown(int number)
{
    const int saved_error = errno;
    if (number != SIGCHLD)
        stop_requested = 1;
    if (signal_pipe >= 0) {
        const char event = 'S';
        const ssize_t ignored = write((int)signal_pipe, &event, 1);
        (void)ignored;
    }
    errno = saved_error;
}

static int configure_descriptor(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static void drain_events(int fd)
{
    char events[64];
    for (;;) {
        const ssize_t result = read(fd, events, sizeof(events));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            break;
    }
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* 只有网络主线程调用 send；每条完整回复共用一个发送期限。 */
static int send_all(int fd, const void *data, size_t length, int64_t deadline)
{
    const char *bytes = data;
    size_t sent = 0;
    while (sent < length) {
        if (stop_requested) {
            errno = ECANCELED;
            return -1;
        }
        const int64_t now = monotonic_ms();
        if (now < 0)
            return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        const ssize_t n = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {fd, POLLOUT, 0};
            /* 最多 100ms 重新检查停止标志，慢客户端不会无限占用服务。 */
            const int remaining = deadline - now > 100 ? 100 : (int)(deadline - now);
            const int result = poll(&descriptor, 1, remaining);
            if (result < 0 && errno != EINTR)
                return -1;
            if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                errno = EPIPE;
                return -1;
            }
            continue;
        }
        if (n == 0)
            errno = EPIPE;
        return -1;
    }
    return 0;
}

static int send_text(int fd, const char *text, size_t length)
{
    const int64_t now = monotonic_ms();
    return now < 0 ? -1 : send_all(fd, text, length, now + SEND_TIMEOUT_MS);
}

static int send_camera_reply(int fd, const struct camera_reply *reply)
{
    char header[128];
    const int jpeg = reply->image.data != NULL;
    const int length = jpeg
        ? snprintf(header, sizeof(header), "JPEG %u %zu\n", reply->id, reply->image.size)
        : snprintf(header, sizeof(header), "%s %u %s\n", reply->status, reply->id, reply->detail);
    if (length < 0 || (size_t)length >= sizeof(header))
        return -1;
    const int64_t now = monotonic_ms();
    if (now < 0 || send_all(fd, header, (size_t)length, now + SEND_TIMEOUT_MS) < 0)
        return -1;
    /* 头部与载荷连续发送，期间不发送任何传感器文本。 */
    return jpeg ? send_all(fd, reply->image.data, reply->image.size, now + SEND_TIMEOUT_MS) : 0;
}

static int parse_request_id(const char *text, size_t length, unsigned int *id)
{
    unsigned int number = 0;
    if (length == 0)
        return -1;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return -1;
        const unsigned int digit = (unsigned int)(text[i] - '0');
        if (number > (UINT_MAX - digit) / 10)
            return -1;
        number = number * 10 + digit;
    }
    if (number == 0)
        return -1;
    *id = number;
    return 0;
}

static const char *const sensor_names[SENSOR_COUNT] = {
    "AP3216C", "ICM20608", "DHT11", "SR04", "SR501"
};

static int send_sensor_reply(int fd, struct sensor_sampler *sampler,
                             enum sensor_kind kind, unsigned int id)
{
    struct sensor_snapshot snapshot;
    char reply[256];
    int length;
    if (sensor_sampler_get(sampler, kind, &snapshot) < 0) {
        length = snprintf(reply, sizeof(reply), "ERR %u %s_READ\n", id, sensor_names[kind]);
    } else {
        const union sensor_value *value = &snapshot.value;
        switch (kind) {
        case SENSOR_AP3216C:
            length = snprintf(reply, sizeof(reply), "AP3216C %u %ld %ld %ld\n", id,
                              value->ap3216c.ir, value->ap3216c.als, value->ap3216c.ps);
            break;
        case SENSOR_ICM20608:
            length = snprintf(reply, sizeof(reply),
                "ICM20608 %u %" PRId32 " %" PRId32 " %" PRId32 " %" PRId32
                " %" PRId32 " %" PRId32 " %" PRId32 "\n", id,
                value->icm20608.gx, value->icm20608.gy, value->icm20608.gz,
                value->icm20608.ax, value->icm20608.ay, value->icm20608.az, value->icm20608.temp);
            break;
        case SENSOR_DHT11:
            length = snprintf(reply, sizeof(reply), "DHT11 %u %u %u\n", id,
                              value->dht11.humidity_tenths, value->dht11.temperature_tenths);
            break;
        case SENSOR_SR04:
            length = snprintf(reply, sizeof(reply), "SR04 %u %u %u\n", id,
                              value->sr04.distance_mm, value->sr04.pulse_us);
            break;
        case SENSOR_SR501:
            length = snprintf(reply, sizeof(reply), "SR501 %u %u\n", id, value->sr501);
            break;
        default: return -1;
        }
    }
    if (length < 0 || (size_t)length >= sizeof(reply))
        return -1;
    return send_text(fd, reply, (size_t)length);
}

static int handle_command(int fd, const char *line, size_t length,
                           struct sensor_sampler *sampler, struct camera_worker *camera)
{
    if (length == 4 && memcmp(line, "PING", 4) == 0)
        return send_text(fd, "PONG\n", 5);

    static const char *const sensor_commands[SENSOR_COUNT] = {
        "READ_AP3216C ", "READ_ICM20608 ", "READ_DHT11 ", "READ_SR04 ", "READ_SR501 "
    };
    for (unsigned int i = 0; i < SENSOR_COUNT; ++i) {
        const size_t prefix = strlen(sensor_commands[i]);
        if (length >= prefix && memcmp(line, sensor_commands[i], prefix) == 0) {
            unsigned int id;
            if (parse_request_id(line + prefix, length - prefix, &id) < 0)
                return send_text(fd, "ERR BAD_REQUEST\n", 16);
            return send_sensor_reply(fd, sampler, (enum sensor_kind)i, id);
        }
    }
    static const char *const camera_commands[] = {
        "CAPTURE_JPEG ", "PREVIEW_START ", "PREVIEW_FRAME ", "PREVIEW_STOP "
    };
    for (unsigned int i = 0; i < sizeof(camera_commands) / sizeof(camera_commands[0]); ++i) {
        const size_t prefix = strlen(camera_commands[i]);
        if (length >= prefix && memcmp(line, camera_commands[i], prefix) == 0) {
            struct camera_request request;
            request.kind = (enum camera_request_kind)i;
            if (parse_request_id(line + prefix, length - prefix, &request.id) < 0)
                return send_text(fd, "ERR BAD_REQUEST\n", 16);
            if (camera_worker_submit(camera, request) == 0)
                return 0;
            const struct camera_reply reply = {request.id, "ERR",
                errno == EAGAIN ? "CAMERA_BUSY" : "CAMERA_START", {0}};
            return send_camera_reply(fd, &reply);
        }
    }
    return send_text(fd, "ERR UNKNOWN_COMMAND\n", 20);
}

static void handle_client(int fd, const char *device, struct sensor_sampler *sampler,
                           int events[2])
{
    struct camera_worker *camera = camera_worker_create(device);
    if (camera == NULL)
        perror("camera_worker_create");
    char line[LINE_CAPACITY];
    size_t used = 0;
    while (!stop_requested) {
        camera_worker_service(camera, 0, 0);
        struct camera_reply reply;
        while (camera_worker_take(camera, &reply) == 1) {
            const int result = send_camera_reply(fd, &reply);
            camera_reply_release(&reply);
            if (result < 0)
                goto cleanup;
        }
        struct pollfd descriptors[4] = {{fd, POLLIN, 0}, {events[0], POLLIN, 0}};
        camera_worker_pollfds(camera, &descriptors[2]);
        const int result = poll(descriptors, 4, 100);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (descriptors[1].revents & POLLIN)
            drain_events(events[0]);
        if (stop_requested)
            break;
        camera_worker_service(camera, descriptors[2].revents, descriptors[3].revents);
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            char buffer[256];
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; ++i) {
                if (buffer[i] == '\n') {
                    line[used] = '\0';
                    if (handle_command(fd, line, used, sampler, camera) < 0)
                        goto cleanup;
                    used = 0;
                } else {
                    if (used >= sizeof(line) - 1)
                        goto cleanup;
                    line[used++] = buffer[i];
                }
            }
        }
        if (descriptors[0].revents & (POLLERR | POLLNVAL))
            break;
    }
cleanup:
    shutdown(fd, SHUT_RDWR);
    if (stop_requested)
        sensor_sampler_request_stop(sampler);
    /* 子进程从未继承客户端 socket；回收后才接受下一连接。 */
    camera_worker_destroy(camera);
}

static int create_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    int reuse = 1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(SERVER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (configure_descriptor(fd) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 5) < 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [/dev/videoX]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *device = argc == 2 ? argv[1] : "/dev/video2";
    setvbuf(stdout, NULL, _IOLBF, 0);
    int events[2];
    if (pipe2(events, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    int exit_status = EXIT_FAILURE;
    int listener = -1;
    struct sensor_sampler *sampler = NULL;
    if (configure_descriptor(events[0]) < 0 || configure_descriptor(events[1]) < 0)
        goto cleanup;
    signal_pipe = events[1];
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGCHLD, &action, NULL) < 0)
        goto cleanup;
    /* 管道读端关闭时由 write 返回 EPIPE，交给相机故障处理，不终止主服务。 */
    action.sa_handler = SIG_IGN;
    action.sa_flags = 0;
    if (sigaction(SIGPIPE, &action, NULL) < 0)
        goto cleanup;
    listener = create_listener();
    if (listener < 0) {
        perror("create_listener");
        goto cleanup;
    }
    sampler = sensor_sampler_create(NULL);
    if (sampler == NULL) {
        perror("sensor_sampler_create");
        goto cleanup;
    }
    printf("采集通信服务监听 IPv4 :%u；5 个传感器线程，连接后创建独立相机子进程。\n",
           (unsigned int)SERVER_PORT);
    exit_status = EXIT_SUCCESS;
    while (!stop_requested) {
        process_manager_reap_lingering();
        struct pollfd descriptors[2] = {{listener, POLLIN, 0}, {events[0], POLLIN, 0}};
        const int result = poll(descriptors, 2, -1);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            exit_status = EXIT_FAILURE;
            break;
        }
        if (descriptors[1].revents & POLLIN)
            drain_events(events[0]);
        if (stop_requested)
            break;
        if (descriptors[0].revents & POLLIN) {
            int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (client < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                exit_status = EXIT_FAILURE;
                break;
            }
            if (configure_descriptor(client) == 0) {
                puts("客户端已连接，创建相机进程通道。");
                handle_client(client, device, sampler, events);
            }
            close(client);
            puts("连接资源已回收，等待下一次连接。");
        }
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            exit_status = EXIT_FAILURE;
            break;
        }
    }
cleanup:
    sensor_sampler_destroy(sampler);
    if (listener >= 0)
        close(listener);
    signal_pipe = -1;
    close(events[1]);
    close(events[0]);
    return exit_status;
}
