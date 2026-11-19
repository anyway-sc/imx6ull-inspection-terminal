#ifndef VEHICLE_PROCESS_MANAGER_H
#define VEHICLE_PROCESS_MANAGER_H

#include <stdint.h>
#include <sys/types.h>

#define PROCESS_STOP_GRACE_MS 500

struct managed_process {
    pid_t pid;
    int fd;
    int stopping, escalated;
    int64_t stop_deadline;
};

int64_t process_monotonic_ms(void);
void process_manager_init(struct managed_process *process);
int process_manager_start(struct managed_process *process, const char *program,
                          const char *device, uint32_t session, uint32_t generation);
/* 仅正常上下文调用：WNOHANG 回收，停止超时后升级为 SIGKILL。 */
int process_manager_poll(struct managed_process *process);
void process_manager_stop(struct managed_process *process);
void process_manager_destroy(struct managed_process *process);
/* 极端内核阻塞留下的进程保留 PID，退出后继续 WNOHANG 回收。 */
void process_manager_reap_lingering(void);

#endif
