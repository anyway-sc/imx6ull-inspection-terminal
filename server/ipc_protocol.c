#include "ipc_protocol.h"
#include <errno.h>

#define IPC_MAGIC 0x5643414du

static void put_word(unsigned char *bytes, uint32_t word)
{
    for (unsigned int i = 0; i < 4; ++i)
        bytes[i] = (unsigned char)(word >> (24 - i * 8));
}

static uint32_t get_word(const unsigned char *bytes)
{
    uint32_t word = 0;
    for (unsigned int i = 0; i < 4; ++i)
        word = (word << 8) | bytes[i];
    return word;
}

void ipc_header_encode(unsigned char bytes[IPC_HEADER_BYTES], const struct ipc_header *header)
{
    const uint32_t words[] = {IPC_MAGIC, IPC_VERSION, header->type, header->session,
        header->generation, header->id, header->code, header->length,
        header->width, header->height};
    for (unsigned int i = 0; i < IPC_HEADER_BYTES / 4; ++i)
        put_word(bytes + i * 4, words[i]);
}

int ipc_header_decode(const unsigned char bytes[IPC_HEADER_BYTES], struct ipc_header *header)
{
    uint32_t words[IPC_HEADER_BYTES / 4];
    for (unsigned int i = 0; i < IPC_HEADER_BYTES / 4; ++i)
        words[i] = get_word(bytes + i * 4);
    if (words[0] != IPC_MAGIC || words[1] != IPC_VERSION ||
        words[2] < IPC_READY || words[2] > IPC_REPLY || words[3] == 0 ||
        words[4] == 0 || words[7] > IPC_MAX_PAYLOAD) {
        errno = EPROTO;
        return -1;
    }
    const struct ipc_header decoded = {words[2], words[3], words[4], words[5],
                                      words[6], words[7], words[8], words[9]};
    *header = decoded;
    return 0;
}
