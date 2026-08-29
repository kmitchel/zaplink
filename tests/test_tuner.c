#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tuner.h"

int g_verbose = 0;

typedef struct {
    Tuner *tuner;
    unsigned long generation;
} AcquireResult;

static long elapsed_milliseconds(const struct timespec *start,
                                 const struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000L +
        (end->tv_nsec - start->tv_nsec) / 1000000L;
}

static void *acquire_stream(void *opaque) {
    AcquireResult *result = opaque;
    result->tuner = acquire_tuner(USER_STREAM, &result->generation);
    return NULL;
}

int main(void) {
    memset(tuners, 0, sizeof(Tuner) * MAX_TUNERS);
    tuner_count = 1;
    tuners[0].id = 0;

    unsigned long epg_generation = 0;
    Tuner *epg = acquire_tuner(USER_EPG, &epg_generation);
    assert(epg == &tuners[0]);
    assert(epg_generation != 0);

    unsigned long stream_generation = 0;
    Tuner *stream = acquire_tuner(USER_STREAM, &stream_generation);
    assert(stream == epg);
    assert(stream_generation != epg_generation);
    assert(stream->user_type == USER_STREAM);

    release_tuner(epg, epg_generation);
    assert(stream->in_use == 1);
    assert(stream->user_type == USER_STREAM);
    assert(tuner_lease_is_current(stream, stream_generation));

    release_tuner(stream, stream_generation);
    assert(stream->in_use == 0);
    assert(stream->user_type == USER_NONE);

    int ready[2];
    assert(pipe(ready) == 0);
    pid_t stubborn = fork();
    assert(stubborn >= 0);
    if (stubborn == 0) {
        close(ready[0]);
        signal(SIGTERM, SIG_IGN);
        assert(write(ready[1], "x", 1) == 1);
        close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);

    unsigned long second_epg_generation = 0;
    epg = acquire_tuner(USER_EPG, &second_epg_generation);
    assert(epg == &tuners[0]);
    assert(tuner_set_process(epg, second_epg_generation, stubborn));

    AcquireResult preempted = {0};
    pthread_t thread;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(pthread_create(&thread, NULL, acquire_stream, &preempted) == 0);
    usleep(100000);

    struct timespec lock_start, lock_end;
    clock_gettime(CLOCK_MONOTONIC, &lock_start);
    assert(!tuner_lease_is_current(epg, second_epg_generation));
    clock_gettime(CLOCK_MONOTONIC, &lock_end);
    assert(elapsed_milliseconds(&lock_start, &lock_end) < 100);

    assert(pthread_join(thread, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &end);
    assert(elapsed_milliseconds(&start, &end) < 2000);
    assert(preempted.tuner == &tuners[0]);
    assert(preempted.generation != second_epg_generation);
    release_tuner(preempted.tuner, preempted.generation);
    assert(tuners[0].user_type == USER_NONE);
    return 0;
}
