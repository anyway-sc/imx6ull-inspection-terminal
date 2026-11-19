#ifndef VEHICLE_CAMERA_WORKER_H
#define VEHICLE_CAMERA_WORKER_H

#include "camera_capture.h"

#define CAMERA_JOB_CAPACITY 4u
#define CAMERA_REPLY_CAPACITY 2u

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
/* notify 只负责唤醒网络线程，不得回调 worker；由工作线程在锁外调用。 */
struct camera_worker *camera_worker_create(const char *device,
                                           void (*notify)(void *), void *context);
/* 非阻塞提交：队列满返回 -1/EAGAIN，不按请求创建线程。 */
int camera_worker_submit(struct camera_worker *worker, struct camera_request request);
/* 有回复返回 1、暂无返回 0；取出后 JPEG 所有权交给调用者。 */
int camera_worker_take(struct camera_worker *worker, struct camera_reply *reply);
void camera_reply_release(struct camera_reply *reply);
/* 丢弃未执行请求，唤醒队列等待，join 后释放未发送 JPEG 和相机。 */
void camera_worker_destroy(struct camera_worker *worker);

#endif
