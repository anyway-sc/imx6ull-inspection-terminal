#ifndef VEHICLE_CAMERA_WORKER_H
#define VEHICLE_CAMERA_WORKER_H

#include "camera_capture.h"

#define CAMERA_JOB_CAPACITY 4u
#define CAMERA_REPLY_CAPACITY 1u

enum camera_request_kind {
    CAMERA_CAPTURE, CAMERA_PREVIEW_START, CAMERA_PREVIEW_FRAME, CAMERA_PREVIEW_STOP
};

struct camera_request {
    enum camera_request_kind kind;
    unsigned int id;
};

struct camera_reply {
    unsigned int id;
    const char *status; /* 文本回复的静态字符串；image.data 非空时为 JPEG。 */
    const char *detail;
    struct camera_jpeg image;
};

struct camera_worker;
struct pollfd;
/* 主进程的 IPC 代理，仅由网络主线程调用。 */
struct camera_worker *camera_worker_create(const char *device);
/* 接入主循环 poll；service 推进部分收发、超时、重启和 waitpid。 */
/* descriptors[0] 读结果，descriptors[1] 写任务，分别监听两根管道。 */
void camera_worker_pollfds(const struct camera_worker *worker, struct pollfd *descriptors);
void camera_worker_service(struct camera_worker *worker, short read_events, short write_events);
/* 非阻塞提交：队列满返回 EAGAIN，子进程不可用返回 ENODEV。 */
int camera_worker_submit(struct camera_worker *worker, struct camera_request request);
/* 有回复返回 1、暂无返回 0；取出后 JPEG 所有权交给调用者。 */
int camera_worker_take(struct camera_worker *worker, struct camera_reply *reply);
void camera_reply_release(struct camera_reply *reply);
/* 关闭 IPC、终止并回收子进程、释放未发送 JPEG；停止期间不重启。 */
void camera_worker_destroy(struct camera_worker *worker);

#endif
