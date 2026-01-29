#ifndef TRANSCODE_H
#define TRANSCODE_H

#include <pthread.h>

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
    OUTPUT_FMP4,
    OUTPUT_INVALID
} TranscodeContainer;

typedef struct {
    char channel_num[16];
    char input_file[256];
    TranscodeBackend backend;
    TranscodeCodec codec;
    TranscodeContainer container;
    int bitrate_kbps;
    int audio_channels;
} StreamConfig;

/**
 * Handle a unified stream request (Transcoded or Raw)
 * Spawns dvbv5-zap (and FFmpeg for transcoding) and pipes output to sockfd.
 * http_header is sent only after the first data chunk arrives.
 */
void handle_unified_stream(int sockfd, StreamConfig *config, const char *http_header);

/**
 * Parse backend and codec strings
 */
TranscodeBackend parse_backend(const char *name);
TranscodeCodec parse_codec(const char *name);

#endif
