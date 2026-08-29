#ifndef STREAM_CONFIG_H
#define STREAM_CONFIG_H

#include <stddef.h>

typedef enum {
    BACKEND_SOFTWARE,
    BACKEND_QSV,
    BACKEND_NVENC,
    BACKEND_VAAPI,
    BACKEND_INVALID
} TranscodeBackend;

typedef enum {
    CODEC_H264,
    CODEC_HEVC,
    CODEC_AV1,
    CODEC_COPY,
    CODEC_INVALID
} TranscodeCodec;

typedef enum {
    OUTPUT_MPEGTS,
    OUTPUT_MATROSKA,
    OUTPUT_INVALID
} TranscodeContainer;

typedef enum {
    LATENCY_LOW,
    LATENCY_BALANCED,
    LATENCY_ROBUST,
    LATENCY_INVALID
} StreamLatency;

typedef struct {
    char channel_num[64];
    TranscodeBackend backend;
    TranscodeCodec codec;
    TranscodeContainer container;
    StreamLatency latency;
    int bitrate_kbps;
    int audio_channels;
    int analyze_duration_us;
    int probe_size_bytes;
    int linger_ms;
    int keyframe_interval_ms;
    int no_buffer;
} StreamConfig;

void stream_config_init(StreamConfig *config);
TranscodeBackend parse_backend(const char *name);
TranscodeCodec parse_codec(const char *name);
TranscodeContainer parse_container(const char *name);
StreamLatency parse_latency(const char *name);

/* Apply profile defaults and resolve the output container. */
int stream_config_finalize(StreamConfig *config,
                           TranscodeContainer path_container,
                           TranscodeContainer query_container);

/* Parse an optional .ts/.mkv suffix without rejecting legacy channel IDs. */
int stream_config_parse_channel_path(const char *path,
                                     char *channel,
                                     size_t channel_size,
                                     TranscodeContainer *path_container);

/* Parse and normalize one stream request. Unknown, duplicate, malformed, and
 * conflicting parameters are rejected with a user-facing error message. */
int stream_config_parse_request(const char *channel_path,
                                const char *query,
                                StreamConfig *config,
                                char *error,
                                size_t error_size);

const char *stream_config_extension(const StreamConfig *config);
const char *stream_config_mime_type(const StreamConfig *config);
const char *stream_latency_name(StreamLatency latency);
int stream_config_equal(const StreamConfig *left, const StreamConfig *right);
int stream_config_shareable(const StreamConfig *config);

#endif
