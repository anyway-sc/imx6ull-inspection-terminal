#define _GNU_SOURCE
#include "process_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static pid_t lingering_pid;

void process_manager_reap_lingering(void)
{
    if (lingering_pid > 0) {
        const pid_t result = waitpid(lingering_pid, NULL, WNOHANG);
        if (result > 0 || (result < 0 && errno == ECHILD)) lingering_pid = 0;
    }
}

int64_t process_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void process_manager_init(struct managed_process *process)
{
    memset(process, 0, sizeof(*process));
    process->fd = -1;
}

int process_manager_start(struct managed_process *process, const char *program,
                          const char *device, uint32_t session, uint32_t generation)
{
    process_manager_reap_lingering();
    if (lingering_pid > 0) { errno = EBUSY; return -1; }
    if (process->pid != 0) { errno = EBUSY; return -1; }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0)
        return -1;
    /* 预留子进程 FD 3；即使标准输入关闭，也不会让 dup2 与源 FD 重合。 */
    const int source = fcntl(pair[1], F_DUPFD_CLOEXEC, 4);
    int error = errno;
    close(pair[1]);
    if (source < 0) { close(pair[0]); errno = error; return -1; }
    const int flags = fcntl(pair[0], F_GETFL);
    if (flags < 0 || fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        error = errno;
        goto fail_descriptors;
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    error = posix_spawn_file_actions_init(&actions);
    if (error != 0) goto fail_descriptors;
    error = posix_spawnattr_init(&attributes);
    if (error != 0) goto fail_actions;
    error = posix_spawn_file_actions_addclose(&actions, pair[0]);
    if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, source, 3);
    if (error == 0) error = posix_spawn_file_actions_addclose(&actions, source);

    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGCHLD);
    if (error == 0) error = posix_spawnattr_setsigmask(&attributes, &empty);
    if (error == 0) error = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (error == 0) error = posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char parent[32], session_text[32], generation_text[32];
    snprintf(parent, sizeof(parent), "%ld", (long)getpid());
    snprintf(session_text, sizeof(session_text), "%u", session);
    snprintf(generation_text, sizeof(generation_text), "%u", generation);
    char *arguments[] = {(char *)program, "3", parent, session_text, generation_text,
                         (char *)device, NULL};
    pid_t pid = 0;
    if (error == 0)
        error = posix_spawn(&pid, program, &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(source);
    if (error != 0) { close(pair[0]); errno = error; return -1; }
    process_manager_init(process);
    process->fd = pair[0];
    process->pid = pid;
    fprintf(stderr, "相机子进程已创建：PID %ld，代次 %u。\n", (long)pid, generation);
    return 0;

fail_actions:
    posix_spawn_file_actions_destroy(&actions);
fail_descriptors:
    close(source);
    close(pair[0]);
    errno = error;
    return -1;
}

void process_manager_stop(struct managed_process *process)
{
    if (process->fd >= 0) {
        shutdown(process->fd, SHUT_RDWR);
        close(process->fd);
        process->fd = -1;
    }
    if (process->pid > 0 && !process->stopping) {
        process->stopping = 1;
        process->stop_deadline = process_monotonic_ms() + PROCESS_STOP_GRACE_MS;
        kill(process->pid, SIGTERM);
    }
}

int process_manager_poll(struct managed_process *process)
{
    if (process->pid == 0) return 0;
    int status;
    pid_t result;
    do { result = waitpid(process->pid, &status, WNOHANG); }
    while (result < 0 && errno == EINTR);
    if (result > 0 || (result < 0 && errno == ECHILD)) {
        fprintf(stderr, "相机子进程已回收：PID %ld。\n", (long)process->pid);
        if (process->fd >= 0) close(process->fd);
        process_manager_init(process);
        return 1;
    }
    if (process->stopping && !process->escalated &&
        process_monotonic_ms() >= process->stop_deadline) {
        kill(process->pid, SIGKILL);
        process->escalated = 1;
    }
    return 0;
}

void process_manager_destroy(struct managed_process *process)
{
    process_manager_stop(process);
    const int64_t deadline = process_monotonic_ms() + 1500;
    while (process->pid > 0) {
        process_manager_poll(process);
        if (process->pid == 0) break;
        if (process_monotonic_ms() >= deadline) {
            /* 不可中断内核等待可能使 SIGKILL 也无法立即生效，禁止永久 waitpid。 */
            fprintf(stderr, "相机进程尚未退出：PID %ld，停止等待。\n", (long)process->pid);
            lingering_pid = process->pid;
            process_manager_init(process);
            break;
        }
        const struct timespec interval = {0, 10000000L};
        nanosleep(&interval, NULL);
    }
}
