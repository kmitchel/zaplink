/**
 * @file log.h
 * @brief Thread-safe, journald-friendly logging with severity levels
 * 
 * Provides macro-based logging with:
 * - Four severity levels: ERROR, WARN, INFO, DEBUG
 * - Automatic timestamps
 * - Syslog priority prefixes understood by journald
 * - Plain key/value fields without terminal escape sequences
 * - Verbose mode gating for DEBUG messages
 * 
 * Usage:
 *   LOG_INFO("HTTP", "Listening on port %d", port);
 *   LOG_ERROR("DB", "Failed to open database: %s", errmsg);
 *   LOG_DEBUG("EPG", "Parsed %d events", count);  // Only with -v flag
 */

#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/** Log severity levels (ordered from most to least critical) */
typedef enum {
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG
} LogLevel;

/** Global verbose flag - controls DEBUG output visibility */
extern int g_verbose;

static inline void log_timestamp(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

static inline void log_write(LogLevel level, const char *component,
                             const char *format, ...) {
    if (level == LOG_LEVEL_DEBUG && !g_verbose) return;

    static const char *names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    static const int priorities[] = {3, 4, 6, 7};
    char timestamp[32];
    log_timestamp(timestamp, sizeof(timestamp));

    flockfile(stderr);
    fprintf(stderr, "<%d>timestamp=%s level=%s component=%s message=",
            priorities[level], timestamp, names[level],
            component ? component : "UNKNOWN");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    funlockfile(stderr);
}

/** Log an error message */
#define LOG_ERROR(tag, ...) log_write(LOG_LEVEL_ERROR, tag, __VA_ARGS__)

/** Log a warning message */
#define LOG_WARN(tag, ...)  log_write(LOG_LEVEL_WARN, tag, __VA_ARGS__)

/** Log an informational message */
#define LOG_INFO(tag, ...)  log_write(LOG_LEVEL_INFO, tag, __VA_ARGS__)

/** Log a debug message (only visible with -v flag) */
#define LOG_DEBUG(tag, ...) log_write(LOG_LEVEL_DEBUG, tag, __VA_ARGS__)

#endif
