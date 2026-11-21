#ifndef VEHICLE_THREAD_UTIL_H
#define VEHICLE_THREAD_UTIL_H

#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>

/* 工作线程使用固定栈上限，不随请求增加线程数量。返回 pthread 错误码。 */
static inline int worker_thread_create(pthread_t *thread,
                                      void *(*entry)(void *), void *argument)
{
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error != 0)
        return error;
    error = pthread_attr_setstacksize(&attributes, 256u * 1024u);
#ifndef _WIN32
    /* 工作线程继承屏蔽集，让 SIGINT/SIGTERM 只由网络主线程处理。 */
    sigset_t signals, previous;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGCHLD);
    const int mask_error = pthread_sigmask(SIG_BLOCK, &signals, &previous);
    if (mask_error != 0) {
        pthread_attr_destroy(&attributes);
        return mask_error;
    }
#endif
    if (error == 0)
        error = pthread_create(thread, &attributes, entry, argument);
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
#endif
    pthread_attr_destroy(&attributes);
    return error;
}

/* 周期等待使用单调时钟，避免系统校时改变采样间隔。 */
static inline int worker_condition_init(pthread_cond_t *condition)
{
#ifdef _WIN32
    /* MinGW 本机测试使用相对等待；其 condattr 不支持 CLOCK_MONOTONIC。 */
    return pthread_cond_init(condition, NULL);
#else
    pthread_condattr_t attributes;
    int error = pthread_condattr_init(&attributes);
    if (error != 0)
        return error;
    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0)
        error = pthread_cond_init(condition, &attributes);
    pthread_condattr_destroy(&attributes);
    return error;
#endif
}

static inline int worker_condition_wait(pthread_cond_t *condition,
                                         pthread_mutex_t *mutex,
                                         const struct timespec *deadline)
{
#ifdef _WIN32
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return errno;
    const int64_t remaining = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000LL
        + deadline->tv_nsec - now.tv_nsec;
    if (remaining <= 0)
        return ETIMEDOUT;
    const struct timespec relative = {(time_t)(remaining / 1000000000LL),
                                     (long)(remaining % 1000000000LL)};
    return pthread_cond_timedwait_relative_np(condition, mutex, &relative);
#else
    return pthread_cond_timedwait(condition, mutex, deadline);
#endif
}

static inline struct timespec timespec_after_ms(struct timespec time,
                                               unsigned int milliseconds)
{
    time.tv_sec += milliseconds / 1000;
    time.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (time.tv_nsec >= 1000000000L) {
        ++time.tv_sec;
        time.tv_nsec -= 1000000000L;
    }
    return time;
}

#endif
