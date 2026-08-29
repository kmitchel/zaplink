#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "stream_session.h"

#define MAX_STREAM_SESSIONS 64
#define SESSION_RING_SIZE (4U * 1024U * 1024U)
#define PRODUCER_POLL_MS 100

typedef enum {
    SESSION_FREE,
    SESSION_STARTING,
    SESSION_RUNNING,
    SESSION_STOPPED
} SessionState;

struct StreamSession {
    SessionState state;
    StreamConfig config;
    pthread_cond_t data_ready;
    unsigned char *ring;
    size_t ring_size;
    uint64_t write_position;
    int subscribers;
    int64_t no_subscribers_since_ms;
    int stop_requested;
    char error[160];
};

static StreamSession sessions[MAX_STREAM_SESSIONS];
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t producers_stopped = PTHREAD_COND_INITIALIZER;
static pthread_once_t sessions_once = PTHREAD_ONCE_INIT;
static StreamProducerOps producer_ops;
static size_t active_producers;
static int initialized;
static int shutting_down;

static int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void initialize_slots(void) {
    for (size_t i = 0; i < MAX_STREAM_SESSIONS; i++) {
        pthread_cond_init(&sessions[i].data_ready, NULL);
    }
}

static void reset_slot(StreamSession *session) {
    free(session->ring);
    session->ring = NULL;
    session->ring_size = 0;
    session->write_position = 0;
    session->subscribers = 0;
    session->no_subscribers_since_ms = 0;
    session->stop_requested = 0;
    session->error[0] = '\0';
    memset(&session->config, 0, sizeof(session->config));
    session->state = SESSION_FREE;
}

static void set_stopped(StreamSession *session, const char *error) {
    pthread_mutex_lock(&sessions_mutex);
    if (error && *error) {
        snprintf(session->error, sizeof(session->error), "%s", error);
    }
    session->state = SESSION_STOPPED;
    if (active_producers > 0) active_producers--;
    pthread_cond_broadcast(&session->data_ready);
    pthread_cond_broadcast(&producers_stopped);
    if (session->subscribers == 0) reset_slot(session);
    pthread_mutex_unlock(&sessions_mutex);
}

static void write_ring(StreamSession *session,
                       const unsigned char *data,
                       size_t length) {
    if (length >= session->ring_size) {
        data += length - session->ring_size;
        length = session->ring_size;
    }
    size_t offset = (size_t)(session->write_position % session->ring_size);
    size_t first = session->ring_size - offset;
    if (first > length) first = length;
    memcpy(session->ring + offset, data, first);
    if (length > first) memcpy(session->ring, data + first, length - first);
    session->write_position += length;
}

static int producer_should_stop(StreamSession *session) {
    int stop = shutting_down || session->stop_requested;
    if (!stop && session->subscribers == 0 &&
        session->no_subscribers_since_ms > 0) {
        int64_t idle = monotonic_milliseconds() - session->no_subscribers_since_ms;
        stop = idle >= session->config.linger_ms;
    }
    return stop;
}

static void *producer_thread(void *argument) {
    StreamSession *session = argument;
    StreamProducer producer = { .fd = -1, .opaque = NULL };
    if (producer_ops.start(&session->config, &producer) != 0 || producer.fd < 0) {
        if (producer.fd >= 0 || producer.opaque) producer_ops.stop(&producer);
        set_stopped(session, "Unable to start stream producer");
        return NULL;
    }

    pthread_mutex_lock(&sessions_mutex);
    if (session->state == SESSION_STARTING) session->state = SESSION_RUNNING;
    pthread_cond_broadcast(&session->data_ready);
    pthread_mutex_unlock(&sessions_mutex);

    unsigned char buffer[65536];
    int ended = 0;
    while (!ended) {
        pthread_mutex_lock(&sessions_mutex);
        int stop = producer_should_stop(session);
        pthread_mutex_unlock(&sessions_mutex);
        if (stop) break;

        struct pollfd descriptor = { .fd = producer.fd, .events = POLLIN };
        int result = poll(&descriptor, 1, PRODUCER_POLL_MS);
        if (result < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (result == 0) continue;

        if (descriptor.revents & POLLIN) {
            ssize_t bytes = read(producer.fd, buffer, sizeof(buffer));
            if (bytes > 0) {
                pthread_mutex_lock(&sessions_mutex);
                write_ring(session, buffer, (size_t)bytes);
                pthread_cond_broadcast(&session->data_ready);
                pthread_mutex_unlock(&sessions_mutex);
            } else if (bytes == 0 || errno != EINTR) {
                ended = 1;
            }
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            while (1) {
                ssize_t bytes = read(producer.fd, buffer, sizeof(buffer));
                if (bytes <= 0) break;
                pthread_mutex_lock(&sessions_mutex);
                write_ring(session, buffer, (size_t)bytes);
                pthread_cond_broadcast(&session->data_ready);
                pthread_mutex_unlock(&sessions_mutex);
            }
            ended = 1;
        }
    }

    producer_ops.stop(&producer);
    set_stopped(session, NULL);
    return NULL;
}

int stream_sessions_init(const StreamProducerOps *ops) {
    if (!ops || !ops->start || !ops->stop) return 0;
    pthread_once(&sessions_once, initialize_slots);
    pthread_mutex_lock(&sessions_mutex);
    if (active_producers != 0) {
        pthread_mutex_unlock(&sessions_mutex);
        return 0;
    }
    producer_ops = *ops;
    initialized = 1;
    shutting_down = 0;
    pthread_mutex_unlock(&sessions_mutex);
    return 1;
}

StreamSession *stream_session_acquire(const StreamConfig *config,
                                      uint64_t *cursor) {
    if (!config || !cursor) return NULL;
    pthread_once(&sessions_once, initialize_slots);
    pthread_mutex_lock(&sessions_mutex);
    if (!initialized || shutting_down) {
        pthread_mutex_unlock(&sessions_mutex);
        return NULL;
    }

    if (stream_config_shareable(config)) {
        for (size_t i = 0; i < MAX_STREAM_SESSIONS; i++) {
            StreamSession *session = &sessions[i];
            if ((session->state == SESSION_STARTING || session->state == SESSION_RUNNING) &&
                stream_config_equal(&session->config, config)) {
                session->subscribers++;
                session->no_subscribers_since_ms = 0;
                *cursor = session->write_position;
                LOG_INFO("SESSION", "Reusing producer for %s (%d subscribers)",
                         config->channel_num, session->subscribers);
                pthread_mutex_unlock(&sessions_mutex);
                return session;
            }
        }
    }

    StreamSession *available = NULL;
    for (size_t i = 0; i < MAX_STREAM_SESSIONS; i++) {
        if (sessions[i].state == SESSION_FREE) {
            available = &sessions[i];
            break;
        }
    }
    if (!available) {
        pthread_mutex_unlock(&sessions_mutex);
        return NULL;
    }

    available->ring = malloc(SESSION_RING_SIZE);
    if (!available->ring) {
        pthread_mutex_unlock(&sessions_mutex);
        return NULL;
    }
    available->ring_size = SESSION_RING_SIZE;
    available->config = *config;
    available->state = SESSION_STARTING;
    available->subscribers = 1;
    available->write_position = 0;
    available->no_subscribers_since_ms = 0;
    available->stop_requested = 0;
    available->error[0] = '\0';
    *cursor = 0;

    pthread_t thread;
    active_producers++;
    if (pthread_create(&thread, NULL, producer_thread, available) != 0) {
        active_producers--;
        reset_slot(available);
        pthread_mutex_unlock(&sessions_mutex);
        return NULL;
    }
    pthread_detach(thread);
    LOG_INFO("SESSION", "Created producer for %s (latency=%s, linger=%dms)",
             config->channel_num, stream_latency_name(config->latency),
             config->linger_ms);
    pthread_mutex_unlock(&sessions_mutex);
    return available;
}

static void realtime_deadline(struct timespec *deadline, int timeout_ms) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

ssize_t stream_session_read(StreamSession *session,
                            uint64_t *cursor,
                            void *buffer,
                            size_t buffer_size,
                            int timeout_ms) {
    if (!session || !cursor || !buffer || buffer_size == 0) return -1;
    struct timespec deadline;
    realtime_deadline(&deadline, timeout_ms);

    pthread_mutex_lock(&sessions_mutex);
    while (*cursor == session->write_position &&
           (session->state == SESSION_STARTING || session->state == SESSION_RUNNING)) {
        int result = pthread_cond_timedwait(&session->data_ready,
                                            &sessions_mutex, &deadline);
        if (result == ETIMEDOUT) {
            pthread_mutex_unlock(&sessions_mutex);
            return 0;
        }
    }

    uint64_t oldest = session->write_position > session->ring_size
        ? session->write_position - session->ring_size : 0;
    if (*cursor < oldest) {
        pthread_mutex_unlock(&sessions_mutex);
        return -2;
    }
    if (*cursor == session->write_position) {
        pthread_mutex_unlock(&sessions_mutex);
        return -1;
    }

    uint64_t available = session->write_position - *cursor;
    size_t length = available < buffer_size ? (size_t)available : buffer_size;
    size_t offset = (size_t)(*cursor % session->ring_size);
    size_t first = session->ring_size - offset;
    if (first > length) first = length;
    memcpy(buffer, session->ring + offset, first);
    if (length > first) memcpy((unsigned char *)buffer + first,
                               session->ring, length - first);
    *cursor += length;
    pthread_mutex_unlock(&sessions_mutex);
    return (ssize_t)length;
}

void stream_session_release(StreamSession *session) {
    if (!session) return;
    pthread_mutex_lock(&sessions_mutex);
    if (session->subscribers > 0) session->subscribers--;
    if (session->subscribers == 0) {
        session->no_subscribers_since_ms = monotonic_milliseconds();
        if (session->state == SESSION_STOPPED) reset_slot(session);
    }
    pthread_cond_broadcast(&session->data_ready);
    pthread_mutex_unlock(&sessions_mutex);
}

const char *stream_session_error(StreamSession *session) {
    if (!session) return "Stream session unavailable";
    return session->error[0] ? session->error : "Stream producer stopped";
}

size_t stream_sessions_active_count(void) {
    pthread_mutex_lock(&sessions_mutex);
    size_t count = active_producers;
    pthread_mutex_unlock(&sessions_mutex);
    return count;
}

void stream_sessions_shutdown(void) {
    pthread_once(&sessions_once, initialize_slots);
    pthread_mutex_lock(&sessions_mutex);
    shutting_down = 1;
    for (size_t i = 0; i < MAX_STREAM_SESSIONS; i++) {
        if (sessions[i].state == SESSION_STARTING || sessions[i].state == SESSION_RUNNING) {
            sessions[i].stop_requested = 1;
            pthread_cond_broadcast(&sessions[i].data_ready);
        }
    }
    while (active_producers > 0) {
        pthread_cond_wait(&producers_stopped, &sessions_mutex);
    }
    for (size_t i = 0; i < MAX_STREAM_SESSIONS; i++) {
        if (sessions[i].subscribers == 0 && sessions[i].state != SESSION_FREE) {
            reset_slot(&sessions[i]);
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
}
