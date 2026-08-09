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
    
    // Sort logic could be added here
    LOG_INFO("TUNER", "Discovered %d tuners", tuner_count);
}

// Internal helper to terminate a process gracefully
static void terminate_process(pid_t pid) {
    if (pid <= 0) return;
    
    // First try SIGTERM for graceful shutdown
    if (kill(pid, SIGTERM) == -1) {
        if (errno == ESRCH) return; // Process doesn't exist
    }
    
    // Wait briefly for process to exit
    int status;
    for (int i = 0; i < 10; i++) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || result == -1) {
            return; // Process exited or error
        }
        usleep(50000); // 50ms
    }
    
    // Process didn't exit gracefully, force kill
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0); // Reap the zombie
}

static unsigned long next_generation(Tuner *t) {
    t->generation++;
    if (t->generation == 0) t->generation++;
    return t->generation;
}

Tuner *acquire_tuner(TunerUser purpose, unsigned long *lease_generation) {
    if (!lease_generation) return NULL;
    pthread_mutex_lock(&tuner_mutex);
    
    if (tuner_count == 0) {
        pthread_mutex_unlock(&tuner_mutex);
        return NULL;
    }

    // 1. Look for idle tuner
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

    // 2. If it's a STREAM request, look for an EPG tuner to preempt
    if (purpose == USER_STREAM) {
        for (int i = 0; i < tuner_count; i++) {
            int idx = (last_tuner_index + 1 + i) % tuner_count;
            if (tuners[idx].user_type == USER_EPG) {
                LOG_DEBUG("TUNER", "Preempting EPG scan on Tuner %d for STREAM", tuners[idx].id);
                
                // Kill the EPG scan process
                // Note: terminate_process reaps the zombie
                if (tuners[idx].zap_pid > 0) {
                    terminate_process(tuners[idx].zap_pid);
                    tuners[idx].zap_pid = 0;
                }
                
                // Keep in_use=1 but change type
                tuners[idx].user_type = USER_STREAM;
                *lease_generation = next_generation(&tuners[idx]);
                last_tuner_index = idx;
                
                pthread_mutex_unlock(&tuner_mutex);
                return &tuners[idx];
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
    pthread_mutex_lock(&tuner_mutex);
    for (int i = 0; i < tuner_count; i++) {
        if (tuners[i].in_use && tuners[i].user_type == purpose &&
            tuners[i].zap_pid > 0) {
            terminate_process(tuners[i].zap_pid);
            tuners[i].zap_pid = 0;
        }
    }
    pthread_mutex_unlock(&tuner_mutex);
}

void release_tuner(Tuner *t, unsigned long lease_generation) {
    if (!t) return;
    
    pthread_mutex_lock(&tuner_mutex);
    
    if (!t->in_use || t->generation != lease_generation) {
        pthread_mutex_unlock(&tuner_mutex);
        LOG_DEBUG("TUNER", "Ignoring stale lease release for tuner %d", t->id);
        return;
    }

    // Terminate child processes and wait to prevent zombies.
    if (t->zap_pid > 0) {
        terminate_process(t->zap_pid);
        t->zap_pid = 0;
    }
    
    t->in_use = 0;
    t->user_type = USER_NONE;
    next_generation(t);
    
    pthread_mutex_unlock(&tuner_mutex);
}
