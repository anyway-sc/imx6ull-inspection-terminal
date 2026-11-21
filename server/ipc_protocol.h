#ifndef VEHICLE_IPC_PROTOCOL_H
#define VEHICLE_IPC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define IPC_HEADER_BYTES 40u
#define IPC_VERSION 1u
#define IPC_MAX_PAYLOAD (4u * 1024u * 1024u)

enum ipc_type { IPC_READY = 1, IPC_REQUEST, IPC_REPLY };
enum ipc_result {
    IPC_JPEG = 1, IPC_PREVIEW_STARTED, IPC_PREVIEW_STOPPED,
    IPC_CAMERA_START, IPC_CAMERA_STOP, IPC_CAMERA_CAPTURE, IPC_CAMERA_FRAME,
    IPC_PREVIEW_NOT_STARTED, IPC_CAMERA_BUSY
};

/* 逻辑字段，不能直接发送此结构体；线上采用固定长度的大端整数。 */
struct ipc_header {
    uint32_t type, session, generation, id, code, length, width, height;
};

void ipc_header_encode(unsigned char bytes[IPC_HEADER_BYTES], const struct ipc_header *header);
int ipc_header_decode(const unsigned char bytes[IPC_HEADER_BYTES], struct ipc_header *header);

#endif
