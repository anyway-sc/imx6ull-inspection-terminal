#include "ap3216c.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* 已验收的板端端口；Windows 通过 ADB 的 19000 入口访问。 */
#define SERVER_PORT 9000
#define LINE_CAPACITY 128

/* 返回 0：全部字节已交给内核；返回 -1：本次发送未完成。 */
static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            perror("send");
            return -1;
        }

        if (n == 0) {
            fprintf(stderr, "send 没有发送进展\n");
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

/* 只接受十进制数字，范围为 1..UINT_MAX；按实际长度检查所有字节。 */
static int parse_request_id(const char *text, size_t length, unsigned int *id)
{
    unsigned int number = 0;

    if (length == 0)
        return -1;

    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return -1;

        unsigned int digit = (unsigned int)(text[i] - '0');
        if (number > (UINT_MAX - digit) / 10)
            return -1;

        number = number * 10 + digit;
    }

    if (number == 0)
        return -1;

    *id = number;
    return 0;
}

static int handle_command(int client_fd, const char *line, size_t length)
{
    const char read_prefix[] = "READ_AP3216C ";
    const size_t prefix_length = sizeof(read_prefix) - 1;

    if (length == 4 && memcmp(line, "PING", 4) == 0) {
        puts("收到 PING，回复 PONG");
        return send_all(client_fd, "PONG\n", 5);
    }

    if (length >= prefix_length &&
        memcmp(line, read_prefix, prefix_length) == 0) {
        unsigned int id;
        if (parse_request_id(line + prefix_length,
                             length - prefix_length, &id) < 0) {
            const char reply[] = "ERR BAD_REQUEST\n";
            return send_all(client_fd, reply, sizeof(reply) - 1);
        }

        struct ap3216c_sample sample;
        char reply[128];
        int n;

        if (ap3216c_read_sample(&sample) < 0) {
            perror("ap3216c_read_sample");
            n = snprintf(reply, sizeof(reply), "ERR %u AP3216C_READ\n", id);
        } else {
            n = snprintf(reply, sizeof(reply), "AP3216C %u %ld %ld %ld\n",
                         id, sample.ir, sample.als, sample.ps);
        }

        if (n < 0 || (size_t)n >= sizeof(reply)) {
            fprintf(stderr, "回复格式化失败或超出缓冲区\n");
            return -1;
        }
        return send_all(client_fd, reply, (size_t)n);
    }

    const char reply[] = "ERR UNKNOWN_COMMAND\n";
    return send_all(client_fd, reply, sizeof(reply) - 1);
}

/* 持续处理一条连接；描述符由调用方 main() 关闭。 */
static void handle_client(int client_fd)
{
    char buf[128];
    char line[LINE_CAPACITY];
    size_t used = 0;

    for (;;) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            perror("recv");
            break;
        }

        if (n == 0) {
            if (used > 0)
                fprintf(stderr, "连接结束，丢弃 %zu 字节未完成命令\n", used);
            break;
        }

        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                line[used] = '\0';
                if (handle_command(client_fd, line, used) < 0)
                    goto cleanup;

                used = 0;
            } else {
                /* 最后一个位置留给 C 字符串结尾的 NUL。 */
                if (used >= sizeof(line) - 1) {
                    fprintf(stderr, "命令超过 %zu 字节，结束当前连接\n",
                            sizeof(line) - 1);
                    goto cleanup;
                }

                line[used++] = buf[i];
            }
        }
    }

cleanup:
    return;
}

/* 成功返回监听描述符；失败时先释放已创建的 socket，再返回 -1。 */
static int create_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 允许服务重启时复用满足系统规则的本地地址。 */
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int main(void)
{
    /* 通过终端或管道查看输出时，每行日志都及时显示。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int listen_fd = create_listener();
    if (listen_fd < 0)
        return EXIT_FAILURE;

    printf("正在监听本地 IPv4 地址的 %u 端口（教学示例）\n",
           (unsigned int)SERVER_PORT);

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;

            perror("accept");
            break;
        }

        puts("客户端已接入");
        handle_client(client_fd);
        close(client_fd);
        puts("当前连接结束，继续等待下一次接入");
    }

    close(listen_fd);
    return EXIT_FAILURE;
}
