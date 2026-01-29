/**
 * @file log.h
 * @brief Colored console logging with severity levels
 * 
 * Provides macro-based logging with:
 * - Four severity levels: ERROR, WARN, INFO, DEBUG
 * - Automatic timestamps
 * - ANSI color coding for readability
 * - Verbose mode gating for DEBUG messages
 * 
 * Usage:
 *   LOG_INFO("HTTP", "Listening on port %d", port);
 *   LOG_ERROR("DB", "Failed to open database: %s", errmsg);
 *   LOG_DEBUG("EPG", "Parsed %d events", count);  // Only with -v flag
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <time.h>

/** Log severity levels (ordered from most to least critical) */
typedef enum {
    LOG_ERROR,  /**< Critical errors that may cause failure */
    LOG_WARN,   /**< Warning conditions */
    LOG_INFO,   /**< Informational messages */
    LOG_DEBUG   /**< Verbose debug output (requires -v flag) */
} LogLevel;

/** Global verbose flag - controls DEBUG output visibility */
extern int g_verbose;

/* ANSI color escape codes for terminal output */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"   /* Errors */
#define COLOR_YELLOW  "\033[1;33m"   /* Warnings */
#define COLOR_GREEN   "\033[1;32m"   /* Info */
#define COLOR_CYAN    "\033[1;36m"   /* Tags */
#define COLOR_DIM     "\033[2m"      /* Debug/timestamps */

/**
 * Format current time as HH:MM:SS into provided buffer (thread-safe)
 */
static inline void log_timestamp(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(buf, len, "%H:%M:%S", &tm_buf);
}

/**
 * Core logging macro - use convenience macros below instead
 */
#define LOG(level, tag, fmt, ...) do { \
    if ((level) == LOG_DEBUG && !g_verbose) break; \
    char _ts[16]; log_timestamp(_ts, sizeof(_ts)); \
    const char *_color = ""; \
    const char *_prefix = ""; \
    switch (level) { \
        case LOG_ERROR: _color = COLOR_RED; _prefix = "ERROR"; break; \
        case LOG_WARN:  _color = COLOR_YELLOW; _prefix = "WARN "; break; \
        case LOG_INFO:  _color = COLOR_GREEN; _prefix = "INFO "; break; \
        case LOG_DEBUG: _color = COLOR_DIM; _prefix = "DEBUG"; break; \
    } \
    fprintf(stderr, "%s[%s]%s %s%-5s%s %s" COLOR_CYAN "%s" COLOR_RESET " " fmt "\n", \
        COLOR_DIM, _ts, COLOR_RESET, \
        _color, _prefix, COLOR_RESET, \
        _color, tag, ##__VA_ARGS__); \
} while(0)

/** Log an error message */
#define LOG_ERROR(tag, fmt, ...) LOG(LOG_ERROR, tag, fmt, ##__VA_ARGS__)

/** Log a warning message */
#define LOG_WARN(tag, fmt, ...)  LOG(LOG_WARN, tag, fmt, ##__VA_ARGS__)

/** Log an informational message */
#define LOG_INFO(tag, fmt, ...)  LOG(LOG_INFO, tag, fmt, ##__VA_ARGS__)

/** Log a debug message (only visible with -v flag) */
#define LOG_DEBUG(tag, fmt, ...) LOG(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)

#endif
