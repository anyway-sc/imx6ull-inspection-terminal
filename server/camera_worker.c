#include "camera_worker.h"
#include "ipc_protocol.h"
#include "process_manager.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAMERA_START_TIMEOUT_MS 3000
#define CAMERA_REQUEST_TIMEOUT_MS 10000
#define CAMERA_SPAWN_LIMIT 3u
#define CAMERA_RESTART_DELAY_MS 500

/* 仅网络主线程调用；不再创建相机线程，队列无需跨线程加锁。 */
struct camera_worker {
    struct managed_process process;
    char *device, *program;
    uint32_t session, generation;
    unsigned int spawn_count;
    int ready, failed, active, reply_ready;
    int64_t deadline, next_start;
    struct camera_request jobs[CAMERA_JOB_CAPACITY], current;
    unsigned int job_head, job_count;
    unsigned int failed_ids[CAMERA_JOB_CAPACITY + 1];
    unsigned int fail_head, fail_count;
    unsigned char tx[IPC_HEADER_BYTES], rx[IPC_HEADER_BYTES];
    size_t tx_used, rx_used, payload_used;
    struct ipc_header incoming;
    struct camera_reply reply;
};

void camera_reply_release(struct camera_reply *reply)
{
    if (reply != NULL) {
        free(reply->image.data);
        memset(reply, 0, sizeof(*reply));
    }
}

static char *service_program(void)
{
    const char *configured = getenv("VEHICLE_CAMERA_SERVICE");
    if (configured != NULL && configured[0] != '\0') return strdup(configured);
    char executable[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (n < 0 || (size_t)n >= sizeof(executable) - 1) { errno = ENAMETOOLONG; return NULL; }
    executable[n] = '\0';
    char *base = strrchr(executable, '/');
    if (base == NULL || (size_t)(base + 1 - executable) + sizeof("camera_service") > sizeof(executable)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(base + 1, "camera_service");
    return strdup(executable);
}

static void fail_worker(struct camera_worker *worker)
{
    if (worker->failed) return;
    worker->failed = 1;
    worker->ready = 0;
    if (worker->active) worker->failed_ids[worker->fail_count++] = worker->current.id;
    for (unsigned int i = 0; i < worker->job_count; ++i)
        worker->failed_ids[worker->fail_count++] =
            worker->jobs[(worker->job_head + i) % CAMERA_JOB_CAPACITY].id;
    worker->active = 0;
    worker->job_count = worker->job_head = 0;
    if (!worker->reply_ready) camera_reply_release(&worker->reply);
    worker->tx_used = worker->rx_used = worker->payload_used = 0;
    worker->next_start = process_monotonic_ms() + CAMERA_RESTART_DELAY_MS;
    process_manager_stop(&worker->process);
    fprintf(stderr, "相机通道失效：取消旧任务，传感器与 TCP 服务继续运行。\n");
}

static void start_worker(struct camera_worker *worker)
{
    ++worker->spawn_count;
    ++worker->generation;
    worker->failed = 0;
    worker->ready = 0;
    worker->rx_used = worker->tx_used = worker->payload_used = 0;
    worker->deadline = process_monotonic_ms() + CAMERA_START_TIMEOUT_MS;
    if (process_manager_start(&worker->process, worker->program, worker->device,
                             worker->session, worker->generation) < 0) {
        perror("camera_service spawn");
        fail_worker(worker);
    }
}

struct camera_worker *camera_worker_create(const char *device)
{
    if (device == NULL || device[0] == '\0') { errno = EINVAL; return NULL; }
    struct camera_worker *worker = calloc(1, sizeof(*worker));
    if (worker == NULL) return NULL;
    process_manager_init(&worker->process);
    worker->device = strdup(device);
    worker->program = service_program();
    if (worker->device == NULL || worker->program == NULL) {
        free(worker->device); free(worker->program); free(worker);
        return NULL;
    }
    static uint32_t next_session;
    if (++next_session == 0) ++next_session;
    worker->session = next_session;
    start_worker(worker);
    return worker;
}

int camera_worker_submit(struct camera_worker *worker, struct camera_request request)
{
    if (worker == NULL || request.id == 0 || request.kind < CAMERA_CAPTURE ||
        request.kind > CAMERA_PREVIEW_STOP) { errno = EINVAL; return -1; }
    if (worker->failed) { errno = ENODEV; return -1; }
    if (worker->job_count == CAMERA_JOB_CAPACITY) { errno = EAGAIN; return -1; }
    worker->jobs[(worker->job_head + worker->job_count) % CAMERA_JOB_CAPACITY] = request;
    ++worker->job_count;
    return 0;
}

void camera_worker_pollfds(const struct camera_worker *worker, struct pollfd *descriptors)
{
    descriptors[0] = (struct pollfd){worker == NULL ? -1 : worker->process.response_fd, POLLIN, 0};
    descriptors[1] = (struct pollfd){worker == NULL ? -1 : worker->process.request_fd,
        worker != NULL && worker->active && worker->tx_used < IPC_HEADER_BYTES ? POLLOUT : 0, 0};
}

static int accept_header(struct camera_worker *worker)
{
    struct ipc_header *header = &worker->incoming;
    if (ipc_header_decode(worker->rx, header) < 0 || header->session != worker->session ||
        header->generation != worker->generation) return -1;
    if (!worker->ready) {
        if (header->type != IPC_READY || header->id != 0 || header->code != 0 ||
            header->length != 0 || header->width != 0 || header->height != 0) return -1;
        worker->ready = 1;
        worker->rx_used = 0;
        return 0;
    }
    if (!worker->active || worker->reply_ready || header->type != IPC_REPLY ||
        header->id != worker->current.id || header->code < IPC_JPEG ||
        header->code > IPC_CAMERA_BUSY) return -1;
    worker->reply.id = header->id;
    worker->reply.status = header->code == IPC_PREVIEW_STARTED ||
        header->code == IPC_PREVIEW_STOPPED || header->code == IPC_JPEG ? "OK" : "ERR";
    static const char *const details[] = {NULL, NULL, "PREVIEW_STARTED", "PREVIEW_STOPPED",
        "CAMERA_START", "CAMERA_STOP", "CAMERA_CAPTURE", "CAMERA_FRAME",
        "PREVIEW_NOT_STARTED", "CAMERA_BUSY"};
    worker->reply.detail = details[header->code];
    if (header->code != IPC_JPEG) {
        if (header->length != 0 || header->width != 0 || header->height != 0 ||
            (header->code == IPC_PREVIEW_STARTED && worker->current.kind != CAMERA_PREVIEW_START) ||
            (header->code == IPC_PREVIEW_STOPPED && worker->current.kind != CAMERA_PREVIEW_STOP))
            return -1;
        worker->reply_ready = 1;
        worker->active = 0;
        worker->rx_used = 0;
        return 0;
    }
    if ((worker->current.kind != CAMERA_CAPTURE && worker->current.kind != CAMERA_PREVIEW_FRAME) ||
        header->length < 4 || header->width == 0 || header->height == 0 ||
        header->width > 4096 || header->height > 4096) return -1;
    worker->reply.image.data = malloc(header->length);
    if (worker->reply.image.data == NULL) return -1;
    worker->reply.image.size = header->length;
    worker->reply.image.width = header->width;
    worker->reply.image.height = header->height;
    return 0;
}

static int receive_reply(struct camera_worker *worker)
{
    /* 完整帧被 take 前不读取下一帧，也不发送下一任务。 */
    while (!worker->reply_ready) {
        unsigned char *destination = worker->rx_used < IPC_HEADER_BYTES
            ? worker->rx + worker->rx_used : worker->reply.image.data + worker->payload_used;
        const size_t remaining = worker->rx_used < IPC_HEADER_BYTES
            ? IPC_HEADER_BYTES - worker->rx_used : worker->incoming.length - worker->payload_used;
        const ssize_t n = read(worker->process.response_fd, destination, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return -1;
        if (worker->rx_used < IPC_HEADER_BYTES) {
            worker->rx_used += (size_t)n;
            if (worker->rx_used == IPC_HEADER_BYTES && accept_header(worker) < 0) return -1;
        } else {
            worker->payload_used += (size_t)n;
            if (worker->payload_used == worker->incoming.length) {
                const unsigned char *image = worker->reply.image.data;
                if (image[0] != 0xff || image[1] != 0xd8 ||
                    image[worker->payload_used - 2] != 0xff || image[worker->payload_used - 1] != 0xd9)
                    return -1;
                worker->reply_ready = 1;
                worker->active = 0;
                worker->rx_used = worker->payload_used = 0;
            }
        }
    }
    return 0;
}

void camera_worker_service(struct camera_worker *worker, short read_events, short write_events)
{
    if (worker == NULL) return;
    if (!worker->failed && (read_events & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
        if (receive_reply(worker) < 0) fail_worker(worker);
    }
    if (!worker->failed && (write_events & (POLLHUP | POLLERR | POLLNVAL)))
        fail_worker(worker);
    if (process_manager_poll(&worker->process) && !worker->failed) fail_worker(worker);
    const int64_t now = process_monotonic_ms();
    if (!worker->failed && (!worker->ready || worker->active) && now >= worker->deadline)
        fail_worker(worker);
    if (worker->failed) {
        if (worker->process.pid == 0 && worker->fail_count == 0 && !worker->reply_ready &&
            worker->spawn_count < CAMERA_SPAWN_LIMIT && now >= worker->next_start)
            start_worker(worker);
        return;
    }
    if (worker->ready && !worker->active && !worker->reply_ready && worker->job_count > 0) {
        worker->current = worker->jobs[worker->job_head];
        worker->job_head = (worker->job_head + 1) % CAMERA_JOB_CAPACITY;
        --worker->job_count;
        worker->active = 1;
        worker->tx_used = 0;
        worker->deadline = now + CAMERA_REQUEST_TIMEOUT_MS;
        const struct ipc_header header = {IPC_REQUEST, worker->session, worker->generation,
            worker->current.id, (uint32_t)worker->current.kind, 0, 0, 0};
        ipc_header_encode(worker->tx, &header);
    }
    while (worker->active && worker->tx_used < IPC_HEADER_BYTES) {
        const ssize_t n = write(worker->process.request_fd, worker->tx + worker->tx_used,
                                IPC_HEADER_BYTES - worker->tx_used);
        if (n > 0) { worker->tx_used += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        fail_worker(worker);
        break;
    }
}

int camera_worker_take(struct camera_worker *worker, struct camera_reply *reply)
{
    if (worker == NULL || reply == NULL) { errno = EINVAL; return -1; }
    if (worker->reply_ready) {
        *reply = worker->reply;
        memset(&worker->reply, 0, sizeof(worker->reply));
        worker->reply_ready = 0;
        return 1;
    }
    if (worker->fail_count > 0) {
        memset(reply, 0, sizeof(*reply));
        reply->id = worker->failed_ids[worker->fail_head++];
        reply->status = "ERR";
        reply->detail = "CAMERA_START";
        if (--worker->fail_count == 0) worker->fail_head = 0;
        return 1;
    }
    return 0;
}

void camera_worker_destroy(struct camera_worker *worker)
{
    if (worker == NULL) return;
    /* destroy 不再执行 service，停止期间不会触发重启。 */
    process_manager_destroy(&worker->process);
    camera_reply_release(&worker->reply);
    free(worker->program);
    free(worker->device);
    free(worker);
}
