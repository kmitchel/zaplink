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
#include "tuner.h"
#include "config.h"
#include "log.h"

/* Global tuner state */
Tuner tuners[MAX_TUNERS];
int tuner_count = 0;
int last_tuner_index = -1;  /* For round-robin selection */

/* Mutex protecting all tuner state modifications */
static pthread_mutex_t tuner_mutex = PTHREAD_MUTEX_INITIALIZER;

void discover_tuners() {
    DIR *d;
    struct dirent *dir;
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
                tuners[tuner_count].id = (int)id;
                snprintf(tuners[tuner_count].path, sizeof(tuners[tuner_count].path), 
                         "/dev/dvb/%s", dir->d_name);
                tuners[tuner_count].in_use = 0;
                tuners[tuner_count].zap_pid = 0;
                tuners[tuner_count].user_type = USER_NONE;
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

Tuner *acquire_tuner(TunerUser purpose) {
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
                last_tuner_index = idx;
                
                pthread_mutex_unlock(&tuner_mutex);
                return &tuners[idx];
            }
        }
    }
    
    pthread_mutex_unlock(&tuner_mutex);
    return NULL;
}

void release_tuner(Tuner *t) {
    if (!t) return;
    
    pthread_mutex_lock(&tuner_mutex);
    
    // Terminate child processes and wait to prevent zombies
    if (t->zap_pid > 0) {
        terminate_process(t->zap_pid);
        t->zap_pid = 0;
    }
    
    t->in_use = 0;
    t->user_type = USER_NONE;
    
    pthread_mutex_unlock(&tuner_mutex);
}
