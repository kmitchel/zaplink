#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "stream_config.h"
#include "stream_session.h"

int g_verbose = 0;

static pthread_mutex_t fake_mutex = PTHREAD_MUTEX_INITIALIZER;
static int writer_fds[16];
static int start_count;
static int stop_count;
static int fail_start;

static int fake_start(const StreamConfig *config, StreamProducer *producer) {
    (void)config;
    if (fail_start) return -1;
    int descriptors[2];
    if (pipe(descriptors) != 0) return -1;
    pthread_mutex_lock(&fake_mutex);
    writer_fds[start_count] = descriptors[1];
    start_count++;
    pthread_mutex_unlock(&fake_mutex);
    producer->fd = descriptors[0];
    producer->opaque = (void *)(intptr_t)descriptors[1];
    return 0;
}

static void fake_stop(StreamProducer *producer) {
    int writer = (int)(intptr_t)producer->opaque;
    if (writer > 0) close(writer);
    pthread_mutex_lock(&fake_mutex);
    stop_count++;
    pthread_mutex_unlock(&fake_mutex);
}

static void wait_for_value(int *value, int expected) {
    for (int attempt = 0; attempt < 50; attempt++) {
        pthread_mutex_lock(&fake_mutex);
        int current = *value;
        pthread_mutex_unlock(&fake_mutex);
        if (current >= expected) return;
        usleep(20000);
    }
    assert(0 && "timed out waiting for producer callback");
}

static void wait_for_no_sessions(void) {
    for (int attempt = 0; attempt < 50; attempt++) {
        if (stream_sessions_active_count() == 0) return;
        usleep(20000);
    }
    assert(0 && "timed out waiting for session cleanup");
}

int main(void) {
    const StreamProducerOps operations = {
        .start = fake_start,
        .stop = fake_stop
    };
    assert(stream_sessions_init(&operations));

    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "21.2");
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));
    config.linger_ms = 120;

    uint64_t first_cursor;
    uint64_t second_cursor;
    StreamSession *first = stream_session_acquire(&config, &first_cursor);
    StreamSession *second = stream_session_acquire(&config, &second_cursor);
    assert(first != NULL);
    assert(first == second);
    wait_for_value(&start_count, 1);
    assert(start_count == 1);

    unsigned char packet[188];
    memset(packet, 0x47, sizeof(packet));
    assert(write(writer_fds[0], packet, sizeof(packet)) == (ssize_t)sizeof(packet));
    unsigned char first_copy[188];
    unsigned char second_copy[188];
    assert(stream_session_read(first, &first_cursor, first_copy,
                               sizeof(first_copy), 1000) == (ssize_t)sizeof(packet));
    assert(stream_session_read(second, &second_cursor, second_copy,
                               sizeof(second_copy), 1000) == (ssize_t)sizeof(packet));
    assert(memcmp(packet, first_copy, sizeof(packet)) == 0);
    assert(memcmp(packet, second_copy, sizeof(packet)) == 0);

    stream_session_release(first);
    stream_session_release(second);

    usleep(40000);
    uint64_t reconnect_cursor;
    StreamSession *reconnected = stream_session_acquire(&config,
                                                        &reconnect_cursor);
    assert(reconnected == first);
    assert(start_count == 1);
    stream_session_release(reconnected);

    wait_for_no_sessions();
    wait_for_value(&stop_count, 1);

    fail_start = 1;
    uint64_t failed_cursor;
    StreamSession *failed = stream_session_acquire(&config, &failed_cursor);
    assert(failed != NULL);
    assert(stream_session_read(failed, &failed_cursor, packet,
                               sizeof(packet), 1000) == -1);
    stream_session_release(failed);
    wait_for_no_sessions();

    fail_start = 0;
    uint64_t recovered_cursor;
    StreamSession *recovered = stream_session_acquire(&config, &recovered_cursor);
    assert(recovered != NULL);
    wait_for_value(&start_count, 2);
    stream_session_release(recovered);
    wait_for_no_sessions();
    wait_for_value(&stop_count, 2);

    stream_sessions_shutdown();
    return 0;
}
