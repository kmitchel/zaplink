/**
 * @file tuner.h
 * @brief DVB tuner resource management
 * 
 * Manages access to DVB tuner hardware (/dev/dvb/adapter*).
 * Implements a priority-based acquisition system where live streams
 * can preempt background EPG scans.
 */

#ifndef TUNER_H
#define TUNER_H

#include <sys/types.h>

/**
 * Purpose for which a tuner is being used
 * Higher priority users can preempt lower priority ones
 */
typedef enum {
    USER_NONE = 0,   /**< Tuner is idle */
    USER_STREAM,     /**< Live streaming (highest priority) */
    USER_EPG         /**< EPG data collection (can be preempted) */
} TunerUser;

/**
 * Represents a single DVB tuner adapter
 */
typedef struct {
    int id;              /**< Adapter number (from /dev/dvb/adapter{id}) */
    char path[512];      /**< Full path to adapter directory */
    int in_use;          /**< Whether tuner is currently acquired */
    pid_t zap_pid;       /**< PID of dvbv5-zap process using this tuner */
    TunerUser user_type; /**< Current usage type */
} Tuner;

/** Array of discovered tuners */
extern Tuner tuners[16];

/** Number of tuners discovered */
extern int tuner_count;

/**
 * Discover available DVB tuners on the system
 * Scans /dev/dvb/ for adapter directories
 */
void discover_tuners();

/**
 * Acquire a tuner for the specified purpose
 * 
 * Acquisition priority:
 * 1. First, try to find an idle tuner
 * 2. If purpose is USER_STREAM, preempt a USER_EPG tuner
 * 
 * @param purpose The intended use for the tuner
 * @return Pointer to acquired Tuner, or NULL if none available
 */
Tuner *acquire_tuner(TunerUser purpose);

/**
 * Release a tuner back to the pool
 * Terminates any child processes (zap) and marks tuner as available
 * @param t Pointer to tuner to release
 */
void release_tuner(Tuner *t);

#endif
