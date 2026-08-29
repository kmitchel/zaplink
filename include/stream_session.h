#ifndef STREAM_SESSION_H
#define STREAM_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "stream_config.h"

typedef struct StreamSession StreamSession;

typedef struct {
    int fd;
    void *opaque;
} StreamProducer;

typedef struct {
    int (*start)(const StreamConfig *config, StreamProducer *producer);
    void (*stop)(StreamProducer *producer);
} StreamProducerOps;

/* Configure producer callbacks before accepting stream requests. */
int stream_sessions_init(const StreamProducerOps *ops);

/* Acquire a subscriber reference and initial read cursor. */
StreamSession *stream_session_acquire(const StreamConfig *config,
                                      uint64_t *cursor);

/*
 * Read broadcast data for one subscriber.
 * Returns bytes read, 0 on timeout, -1 when stopped, or -2 on overrun.
 */
ssize_t stream_session_read(StreamSession *session,
                            uint64_t *cursor,
                            void *buffer,
                            size_t buffer_size,
                            int timeout_ms);

void stream_session_release(StreamSession *session);
const char *stream_session_error(StreamSession *session);
size_t stream_sessions_active_count(void);
void stream_sessions_shutdown(void);

#endif
