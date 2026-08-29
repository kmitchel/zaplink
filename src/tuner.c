/**
 * @file tuner.c
 * @brief DVB tuner resource management implementation
 * 
 * Manages exclusive access to DVB tuner hardware. Key features:
 * 
 * - Discovery: Scans /dev/dvb/adapter* for available tuners
 * - Acquisition: Thread-safe tuner locking with round-robin selection
 * - Preemption: Stream requests can preempt background EPG scans
 * - Cleanup: Graceful process termination (SIGTERM then SIGKILL)
 * 
 * Thread safety: All acquisition/release operations are protected
 * by a mutex to prevent race conditions between stream handlers
 * and EPG worker threads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include <glob.h>
#include "tuner.h"
#include "config.h"
#include "log.h"

/* Global tuner state */
Tuner tuners[MAX_TUNERS];
int tuner_count = 0;
int last_tuner_index = -1;  /* For round-robin selection */

/* Mutex protecting all tuner state modifications */
static pthread_mutex_t tuner_mutex = PTHREAD_MUTEX_INITIALIZER;

static int adapter_has_usable_frontend(const char *adapter_path) {
    char pattern[1024];
    glob_t matches;
    int usable = 0;

    if (snprintf(pattern, sizeof(pattern), "%s/frontend*", adapter_path) >=
        (int)sizeof(pattern)) {
        return 0;
    }
    if (glob(pattern, GLOB_NOSORT, NULL, &matches) != 0) return 0;

    for (size_t i = 0; i < matches.gl_pathc; i++) {
        if (access(matches.gl_pathv[i], R_OK | W_OK) == 0) {
            usable = 1;
            break;
        }
    }
    globfree(&matches);
    return usable;
}

static int count_usable_frontends(void) {
    glob_t matches;
    int count = 0;

    if (glob("/dev/dvb/adapter*/frontend*", GLOB_NOSORT, NULL, &matches) != 0) {
        return 0;
    }

    for (size_t i = 0; i < matches.gl_pathc; i++) {
        if (access(matches.gl_pathv[i], R_OK | W_OK) == 0) {
            count++;
        }
    }

    globfree(&matches);
    return count;
}

int wait_for_tuners(unsigned int timeout_seconds) {
    const useconds_t poll_interval_us = 250000;
    unsigned int waited_ms = 0;
    int count = count_usable_frontends();

    if (count > 0) {
        LOG_INFO("TUNER", "%d usable DVB frontend%s ready", count,
                 count == 1 ? "" : "s");
        return count;
    }

    LOG_INFO("TUNER", "Waiting up to %u seconds for DVB frontends",
             timeout_seconds);

    while (waited_ms < timeout_seconds * 1000U) {
        usleep(poll_interval_us);
        waited_ms += poll_interval_us / 1000U;
        count = count_usable_frontends();
        if (count > 0) {
            LOG_INFO("TUNER", "%d usable DVB frontend%s ready after %.2f seconds",
                     count, count == 1 ? "" : "s", waited_ms / 1000.0);
            return count;
        }
    }

    LOG_WARN("TUNER", "Timed out after %u seconds waiting for DVB frontends",
             timeout_seconds);
    return 0;
}

void discover_tuners() {
    DIR *d;
    struct dirent *dir;
    tuner_count = 0;
    last_tuner_index = -1;

    d = opendir("/dev/dvb");
    if (!d) {
        LOG_WARN("TUNER", "/dev/dvb not found");
        return;
    }

    while ((dir = readdir(d)) != NULL) {
        if (strncmp(dir->d_name, "adapter", 7) == 0) {
            // Validate that the rest of the name is a number
            char *endptr;
            long id = strtol(dir->d_name + 7, &endptr, 10);
            if (*endptr != '\0' || id < 0 || id > 999) {
                // Invalid adapter name, skip
                continue;
            }
            
            if (tuner_count < MAX_TUNERS) {
                snprintf(tuners[tuner_count].path,
                         sizeof(tuners[tuner_count].path),
                         "/dev/dvb/%s", dir->d_name);
                if (!adapter_has_usable_frontend(tuners[tuner_count].path)) continue;

                tuners[tuner_count].id = (int)id;
                tuners[tuner_count].in_use = 0;
                tuners[tuner_count].zap_pid = 0;
                tuners[tuner_count].user_type = USER_NONE;
                tuners[tuner_count].generation = 0;
                tuner_count++;
            }
        }
    }
    closedir(d);
    
    LOG_INFO("TUNER", "Discovered %d tuners", tuner_count);
}

static int wait_for_process(pid_t pid, int attempts) {
    int status;
    for (int i = 0; i < attempts; i++) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) return 1;
        if (result < 0 && errno != EINTR) return 0;
        usleep(50000);
    }
    return 0;
}

/* This function is intentionally bounded and is never called with
 * tuner_mutex held. */
static int terminate_process(pid_t pid) {
    if (pid <= 0) return 1;
    if (kill(pid, SIGTERM) < 0 && errno == ESRCH) return 1;
    if (wait_for_process(pid, 10)) return 1;
    if (kill(pid, SIGKILL) < 0 && errno == ESRCH) return 1;
    return wait_for_process(pid, 10);
}

static unsigned long next_generation(Tuner *t) {
    t->generation++;
    if (t->generation == 0) t->generation++;
    return t->generation;
}

typedef struct {
    Tuner *tuner;
    unsigned long generation;
    pid_t pid;
} TerminationReaper;

static void *reap_terminated_process(void *opaque) {
    TerminationReaper *reaper = opaque;
    int status;
    pid_t result;
    do {
        result = waitpid(reaper->pid, &status, 0);
    } while (result < 0 && errno == EINTR);

    if (result == reaper->pid || (result < 0 && errno == ECHILD)) {
        pthread_mutex_lock(&tuner_mutex);
        if (reaper->tuner->generation == reaper->generation &&
            reaper->tuner->user_type == USER_STOPPING) {
            reaper->tuner->zap_pid = 0;
            reaper->tuner->in_use = 0;
            reaper->tuner->user_type = USER_NONE;
            next_generation(reaper->tuner);
            LOG_INFO("TUNER", "Quarantined tuner %d is available again",
                     reaper->tuner->id);
        }
        pthread_mutex_unlock(&tuner_mutex);
    }
    free(reaper);
    return NULL;
}

static void schedule_reaper(Tuner *tuner, unsigned long generation, pid_t pid) {
    TerminationReaper *reaper = malloc(sizeof(*reaper));
    if (!reaper) {
        LOG_ERROR("TUNER", "Unable to monitor stuck process %d", pid);
        return;
    }
    *reaper = (TerminationReaper) {
        .tuner = tuner, .generation = generation, .pid = pid
    };
    pthread_t thread;
    int result = pthread_create(&thread, NULL, reap_terminated_process, reaper);
    if (result != 0) {
        LOG_ERROR("TUNER", "Unable to create process reaper for %d: %s",
                  pid, strerror(result));
        free(reaper);
        return;
    }
    pthread_detach(thread);
}

Tuner *acquire_tuner(TunerUser purpose, unsigned long *lease_generation) {
    if (!lease_generation) return NULL;
    pthread_mutex_lock(&tuner_mutex);
    
    if (tuner_count == 0) {
        pthread_mutex_unlock(&tuner_mutex);
        return NULL;
    }

    for (int i = 0; i < tuner_count; i++) {
        int idx = (last_tuner_index + 1 + i) % tuner_count;
        if (!tuners[idx].in_use) {
            tuners[idx].in_use = 1;
            tuners[idx].user_type = purpose;
            *lease_generation = next_generation(&tuners[idx]);
            last_tuner_index = idx;
            pthread_mutex_unlock(&tuner_mutex);
            return &tuners[idx];
        }
    }

    if (purpose == USER_STREAM) {
        for (int i = 0; i < tuner_count; i++) {
            int idx = (last_tuner_index + 1 + i) % tuner_count;
            if (tuners[idx].user_type == USER_EPG) {
                Tuner *tuner = &tuners[idx];
                pid_t pid = tuner->zap_pid;
                tuner->zap_pid = 0;
                tuner->user_type = USER_STOPPING;
                unsigned long transition = next_generation(tuner);
                pthread_mutex_unlock(&tuner_mutex);

                if (pid > 0) {
                    LOG_INFO("TUNER", "Preempting EPG process %d on tuner %d",
                             pid, tuner->id);
                }
                int stopped = terminate_process(pid);
                if (!stopped) schedule_reaper(tuner, transition, pid);

                pthread_mutex_lock(&tuner_mutex);
                if (!stopped || tuner->generation != transition ||
                    tuner->user_type != USER_STOPPING) {
                    if (!stopped && tuner->generation == transition) {
                        tuner->zap_pid = pid;
                        LOG_ERROR("TUNER",
                                  "Process %d did not stop; tuner %d quarantined",
                                  pid, tuner->id);
                    }
                    pthread_mutex_unlock(&tuner_mutex);
                    return NULL;
                }
                tuner->user_type = USER_STREAM;
                *lease_generation = next_generation(tuner);
                last_tuner_index = idx;
                pthread_mutex_unlock(&tuner_mutex);
                return tuner;
            }
        }
    }
    
    pthread_mutex_unlock(&tuner_mutex);
    return NULL;
}

int tuner_set_process(Tuner *t, unsigned long lease_generation, pid_t pid) {
    if (!t || pid <= 0) return 0;

    pthread_mutex_lock(&tuner_mutex);
    int current = t->in_use && t->generation == lease_generation;
    if (current) t->zap_pid = pid;
    pthread_mutex_unlock(&tuner_mutex);
    return current;
}

void tuner_clear_process(Tuner *t, unsigned long lease_generation, pid_t pid) {
    if (!t) return;

    pthread_mutex_lock(&tuner_mutex);
    if (t->in_use && t->generation == lease_generation &&
        (pid <= 0 || t->zap_pid == pid)) {
        t->zap_pid = 0;
    }
    pthread_mutex_unlock(&tuner_mutex);
}

int tuner_lease_is_current(Tuner *t, unsigned long lease_generation) {
    if (!t) return 0;
    pthread_mutex_lock(&tuner_mutex);
    int current = t->in_use && t->generation == lease_generation;
    pthread_mutex_unlock(&tuner_mutex);
    return current;
}

void cancel_tuner_users(TunerUser purpose) {
    for (int i = 0; i < tuner_count; i++) {
        pthread_mutex_lock(&tuner_mutex);
        if (tuners[i].in_use && tuners[i].user_type == purpose &&
            tuners[i].zap_pid > 0) {
            pid_t pid = tuners[i].zap_pid;
            tuners[i].zap_pid = 0;
            tuners[i].user_type = USER_STOPPING;
            unsigned long transition = next_generation(&tuners[i]);
            pthread_mutex_unlock(&tuner_mutex);

            int stopped = terminate_process(pid);
            if (!stopped) schedule_reaper(&tuners[i], transition, pid);
            pthread_mutex_lock(&tuner_mutex);
            if (tuners[i].generation == transition &&
                tuners[i].user_type == USER_STOPPING) {
                if (stopped) {
                    tuners[i].in_use = 0;
                    tuners[i].user_type = USER_NONE;
                    next_generation(&tuners[i]);
                } else {
                    tuners[i].zap_pid = pid;
                    LOG_ERROR("TUNER",
                              "Process %d did not stop; tuner %d quarantined",
                              pid, tuners[i].id);
                }
            }
        }
        pthread_mutex_unlock(&tuner_mutex);
    }
}

void release_tuner(Tuner *t, unsigned long lease_generation) {
    if (!t) return;
    
    pthread_mutex_lock(&tuner_mutex);
    
    if (!t->in_use || t->generation != lease_generation) {
        pthread_mutex_unlock(&tuner_mutex);
        LOG_DEBUG("TUNER", "Ignoring stale lease release for tuner %d", t->id);
        return;
    }

    pid_t pid = t->zap_pid;
    t->zap_pid = 0;
    t->user_type = USER_STOPPING;
    unsigned long transition = next_generation(t);
    pthread_mutex_unlock(&tuner_mutex);

    int stopped = terminate_process(pid);
    if (!stopped) schedule_reaper(t, transition, pid);

    pthread_mutex_lock(&tuner_mutex);
    if (t->generation == transition && t->user_type == USER_STOPPING) {
        if (stopped) {
            t->in_use = 0;
            t->user_type = USER_NONE;
            next_generation(t);
        } else {
            t->zap_pid = pid;
            LOG_ERROR("TUNER", "Process %d did not stop; tuner %d quarantined",
                      pid, t->id);
        }
    }
    pthread_mutex_unlock(&tuner_mutex);
}
