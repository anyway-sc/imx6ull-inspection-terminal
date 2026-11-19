#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include <stddef.h>

/* 限制单张 JPEG 的传输大小；两端应采用相同上限。 */
#define CAMERA_MAX_JPEG_BYTES (4u * 1024u * 1024u)

struct camera_jpeg {
    unsigned char *data;
    size_t size;
    unsigned int width;
    unsigned int height;
};

/* 句柄先设 NULL；open 一次、read 多次，结束或读取失败时 close。 */
struct camera_stream;
int camera_stream_open(const char *device, struct camera_stream **stream);
/* image 先用 {0} 初始化；每次读取成功后由调用者 release。 */
int camera_stream_read(struct camera_stream *stream, struct camera_jpeg *image);
/* 释放设备与缓冲，句柄设 NULL；允许重复调用。 */
int camera_stream_close(struct camera_stream **stream);

/* image 必须先用 {0} 初始化；成功后由调用者释放，失败时不改输出。 */
int camera_capture_jpeg(const char *device, struct camera_jpeg *image);
void camera_jpeg_release(struct camera_jpeg *image);

#endif
