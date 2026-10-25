#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "stream_session.h"
int g_verbose = 0;
static int mode, initialized_conditions, destroyed_conditions;
int __real_pthread_condattr_init(pthread_condattr_t *);
int __real_pthread_condattr_setclock(pthread_condattr_t *, clockid_t);
int __real_pthread_condattr_destroy(pthread_condattr_t *);
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int __real_pthread_cond_destroy(pthread_cond_t *);
int __wrap_pthread_condattr_init(pthread_condattr_t *attr) {
    return mode == 1 ? ENOMEM : __real_pthread_condattr_init(attr);
}
int __wrap_pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock) {
    return mode == 2 ? EINVAL : __real_pthread_condattr_setclock(attr, clock);
}
int __wrap_pthread_condattr_destroy(pthread_condattr_t *attr) {
    int result = __real_pthread_condattr_destroy(attr);
    return mode == 3 ? EINVAL : result;
}
int __wrap_pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    if (mode == 4 && initialized_conditions == 3) return ENOMEM;
    int result = __real_pthread_cond_init(cond, attr);
    if (!result) initialized_conditions++;
    return result;
}
int __wrap_pthread_cond_destroy(pthread_cond_t *cond) {
    destroyed_conditions++;
    return __real_pthread_cond_destroy(cond);
}
static int unused_start(const StreamConfig *config, StreamProducer *producer) {
    (void)config; (void)producer; assert(0); return -1;
}
static void unused_stop(StreamProducer *producer) { (void)producer; assert(0); }
int main(void) {
    for (mode = 1; mode <= 4; mode++) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            StreamProducerOps ops = {.start=unused_start, .stop=unused_stop};
            assert(!stream_sessions_init(&ops));
            StreamConfig config; stream_config_init(&config); uint64_t cursor;
            assert(!stream_session_acquire(&config, &cursor));
            stream_sessions_shutdown();
            assert(initialized_conditions == destroyed_conditions);
            _exit(0);
        }
        int status; assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("session condition initialization failure tests: OK");
}
