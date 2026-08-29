#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "stream_config.h"
#include "stream_session.h"

int g_verbose = 0;

static pthread_mutex_t fake_mutex = PTHREAD_MUTEX_INITIALIZER;
static int writer_fds[128];
static int start_count;
static int stop_count;
static int fail_start;

static int fake_start(const StreamConfig *config, StreamProducer *producer) {
    (void)config;
    if (fail_start) {
        strcpy(producer->error, "controlled producer startup failure");
        return -1;
    }
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

typedef struct {
    StreamConfig *config;
    StreamSession *session;
    uint64_t cursor;
} ConcurrentAcquire;

static void *acquire_concurrently(void *opaque) {
    ConcurrentAcquire *request = opaque;
    request->session = stream_session_acquire(request->config,
                                              &request->cursor);
    return NULL;
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
    assert(strcmp(stream_session_error(failed),
                  "controlled producer startup failure") == 0);
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

    enum { CONCURRENT_CLIENTS = 16 };
    ConcurrentAcquire concurrent[CONCURRENT_CLIENTS];
    pthread_t threads[CONCURRENT_CLIENTS];
    for (int index = 0; index < CONCURRENT_CLIENTS; index++) {
        concurrent[index].config = &config;
        concurrent[index].session = NULL;
        assert(pthread_create(&threads[index], NULL, acquire_concurrently,
                              &concurrent[index]) == 0);
    }
    for (int index = 0; index < CONCURRENT_CLIENTS; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
        assert(concurrent[index].session != NULL);
        assert(concurrent[index].session == concurrent[0].session);
    }
    wait_for_value(&start_count, 3);
    assert(start_count == 3);
    for (int index = 0; index < CONCURRENT_CLIENTS; index++) {
        stream_session_release(concurrent[index].session);
    }
    wait_for_no_sessions();

    StreamConfig isolated = config;
    isolated.codec = CODEC_H264;
    isolated.audio_channels = 6;
    assert(stream_config_finalize(&isolated, OUTPUT_MPEGTS, OUTPUT_INVALID));
    isolated.linger_ms = 0;
    config.codec = CODEC_H264;
    config.audio_channels = 2;
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));
    config.linger_ms = 0;
    uint64_t isolated_cursor;
    StreamSession *base_session = stream_session_acquire(&config, &first_cursor);
    StreamSession *isolated_session = stream_session_acquire(&isolated,
                                                              &isolated_cursor);
    assert(base_session && isolated_session && base_session != isolated_session);
    stream_session_release(base_session);
    stream_session_release(isolated_session);
    wait_for_no_sessions();

    StreamConfig slots[65];
    StreamSession *slot_sessions[65] = {0};
    uint64_t slot_cursors[65];
    for (int index = 0; index < 65; index++) {
        stream_config_init(&slots[index]);
        snprintf(slots[index].channel_num, sizeof(slots[index].channel_num),
                 "%d.1", index + 1);
        slots[index].codec = CODEC_H264;
        assert(stream_config_finalize(&slots[index], OUTPUT_MATROSKA,
                                      OUTPUT_INVALID));
        slot_sessions[index] = stream_session_acquire(&slots[index],
                                                       &slot_cursors[index]);
        if (index < 64) assert(slot_sessions[index] != NULL);
    }
    assert(slot_sessions[64] == NULL);
    for (int index = 0; index < 64; index++) {
        stream_session_release(slot_sessions[index]);
    }
    wait_for_no_sessions();

    stream_config_init(&config);
    strcpy(config.channel_num, "21.2");
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));
    config.linger_ms = 0;
    StreamSession *overrun = stream_session_acquire(&config, &first_cursor);
    assert(overrun != NULL);
    wait_for_value(&start_count, 70);
    unsigned char block[65536];
    memset(block, 0x47, sizeof(block));
    int overrun_writer = writer_fds[start_count - 1];
    for (int index = 0; index < 72; index++) {
        assert(write(overrun_writer, block, sizeof(block)) ==
               (ssize_t)sizeof(block));
    }
    usleep(200000);
    assert(stream_session_read(overrun, &first_cursor, block, sizeof(block),
                               1000) == -2);
    stream_session_release(overrun);
    wait_for_no_sessions();

    StreamSession *active = stream_session_acquire(&config, &first_cursor);
    assert(active != NULL);
    stream_sessions_shutdown();
    assert(stream_sessions_active_count() == 0);
    stream_session_release(active);

    return 0;
}
