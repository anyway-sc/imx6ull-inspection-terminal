#define _GNU_SOURCE
#include "ipc_protocol.h"
#include "process_manager.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned int pipe_calls, fail_pipe_call;
int __real_pipe2(int descriptors[2], int flags);

int __wrap_pipe2(int descriptors[2], int flags)
{
    if (++pipe_calls == fail_pipe_call) { errno = EMFILE; return -1; }
    return __real_pipe2(descriptors, flags);
}

static unsigned int descriptor_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory != NULL);
    unsigned int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
        if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}

static void expect_ready(struct managed_process *process)
{
    unsigned char wire[IPC_HEADER_BYTES];
    size_t used = 0;
    const int64_t deadline = process_monotonic_ms() + 3000;
    while (used < sizeof(wire)) {
        assert(process_monotonic_ms() < deadline);
        const ssize_t n = read(process->response_fd, wire + used, sizeof(wire) - used);
        if (n > 0) { used += (size_t)n; continue; }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
        struct pollfd fd = {process->response_fd, POLLIN, 0};
        assert(poll(&fd, 1, 100) >= 0 || errno == EINTR);
    }
    struct ipc_header header;
    assert(ipc_header_decode(wire, &header) == 0 && header.type == IPC_READY);
    assert(header.session == 1 && header.generation == 1);
}

static void expect_normal_exit(struct managed_process *process)
{
    int status = 0;
    const int64_t deadline = process_monotonic_ms() + 3000;
    for (;;) {
        const pid_t result = waitpid(process->pid, &status, WNOHANG);
        if (result == process->pid) break;
        assert(result == 0 || (result < 0 && errno == EINTR));
        assert(process_monotonic_ms() < deadline);
        const struct timespec interval = {0, 10000000L};
        nanosleep(&interval, NULL);
    }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    process->pid = 0;
    process_manager_destroy(process);
}

static void check_start_failures(const char *program)
{
    struct managed_process process;
    process_manager_init(&process);
    const unsigned int baseline = descriptor_count();
    for (fail_pipe_call = 1; fail_pipe_call <= 2; ++fail_pipe_call) {
        pipe_calls = 0;
        assert(process_manager_start(&process, program, "fake", 1, 1) < 0 && errno == EMFILE);
        assert(descriptor_count() == baseline && process.pid == 0);
    }
    fail_pipe_call = 0;
    assert(process_manager_start(&process, "/no-such-vehicle-camera", "fake", 1, 1) < 0);
    assert(descriptor_count() == baseline && process.pid == 0);
    puts("通过：第一/第二根管道创建失败及 spawn 失败后无 FD 泄漏");
}

static void check_descriptor_mapping(const char *program)
{
    struct managed_process process;
    process_manager_init(&process);
    const unsigned int baseline = descriptor_count();
    int saved[3];
    for (unsigned int i = 0; i < 3; ++i) {
        saved[i] = fcntl((int)i, F_DUPFD_CLOEXEC, 10);
        assert(saved[i] >= 10);
        close((int)i);
    }
    const int result = process_manager_start(&process, program, "fake", 1, 1);
    if (result == 0) {
        const int request = fcntl(process.request_fd, F_DUPFD_CLOEXEC, 5);
        const int response = fcntl(process.response_fd, F_DUPFD_CLOEXEC, 5);
        assert(request >= 5 && response >= 5);
        close(process.request_fd);
        close(process.response_fd);
        process.request_fd = request;
        process.response_fd = response;
    }
    for (unsigned int i = 0; i < 3; ++i) {
        assert(dup2(saved[i], (int)i) == (int)i);
        close(saved[i]);
    }
    clearerr(stderr);
    assert(result == 0);
    assert(descriptor_count() == baseline + 2);
    const int endpoints[] = {process.request_fd, process.response_fd};
    for (unsigned int i = 0; i < 2; ++i) {
        struct stat information;
        assert(fstat(endpoints[i], &information) == 0 && S_ISFIFO(information.st_mode));
        const int flags = fcntl(endpoints[i], F_GETFL);
        assert(flags >= 0 && (flags & O_NONBLOCK));
        assert((fcntl(endpoints[i], F_GETFD) & FD_CLOEXEC) != 0);
    }
    expect_ready(&process);
    close(process.request_fd);
    process.request_fd = -1;
    expect_normal_exit(&process);
    assert(descriptor_count() == baseline);
    puts("通过：标准流关闭时端点映射正确，任务写端关闭后相机读到 EOF 并退出");
}

static void check_closed_response(const char *program)
{
    struct managed_process process;
    process_manager_init(&process);
    const unsigned int baseline = descriptor_count();
    assert(process_manager_start(&process, program, "fake", 1, 1) == 0);
    expect_ready(&process);
    close(process.response_fd);
    process.response_fd = -1;
    const struct ipc_header request = {IPC_REQUEST, 1, 1, 1, 0, 0, 0, 0};
    unsigned char wire[IPC_HEADER_BYTES];
    ipc_header_encode(wire, &request);
    assert(write(process.request_fd, wire, sizeof(wire)) == (ssize_t)sizeof(wire));
    expect_normal_exit(&process);
    assert(descriptor_count() == baseline);
    puts("通过：结果读端关闭后相机处理 EPIPE，正常释放并退出而非被 SIGPIPE 终止");
}

int main(int argc, char *argv[])
{
    assert(argc == 2);
    check_start_failures(argv[1]);
    check_descriptor_mapping(argv[1]);
    check_closed_response(argv[1]);
    return 0;
}
