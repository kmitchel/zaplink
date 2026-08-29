#include <string.h>
#include <strings.h>

#include "stream_config.h"

void stream_config_init(StreamConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->backend = BACKEND_SOFTWARE;
    config->codec = CODEC_COPY;
    config->container = OUTPUT_INVALID;
    config->latency = LATENCY_BALANCED;
    config->audio_channels = 2;
}

TranscodeBackend parse_backend(const char *name) {
    if (!name) return BACKEND_SOFTWARE;
    if (strcasecmp(name, "qsv") == 0) return BACKEND_QSV;
    if (strcasecmp(name, "nvenc") == 0) return BACKEND_NVENC;
    if (strcasecmp(name, "vaapi") == 0) return BACKEND_VAAPI;
    if (strcasecmp(name, "software") == 0) return BACKEND_SOFTWARE;
    return BACKEND_INVALID;
}

TranscodeCodec parse_codec(const char *name) {
    if (!name) return CODEC_COPY;
    if (strcasecmp(name, "h264") == 0) return CODEC_H264;
    if (strcasecmp(name, "hevc") == 0 || strcasecmp(name, "h265") == 0) return CODEC_HEVC;
    if (strcasecmp(name, "av1") == 0) return CODEC_AV1;
    if (strcasecmp(name, "copy") == 0) return CODEC_COPY;
    return CODEC_INVALID;
}

TranscodeContainer parse_container(const char *name) {
    if (!name || !*name) return OUTPUT_INVALID;
    if (strcasecmp(name, "ts") == 0 || strcasecmp(name, "mpegts") == 0) {
        return OUTPUT_MPEGTS;
    }
    if (strcasecmp(name, "mkv") == 0 || strcasecmp(name, "matroska") == 0) {
        return OUTPUT_MATROSKA;
    }
    return OUTPUT_INVALID;
}

StreamLatency parse_latency(const char *name) {
    if (!name || !*name || strcasecmp(name, "balanced") == 0) return LATENCY_BALANCED;
    if (strcasecmp(name, "low") == 0) return LATENCY_LOW;
    if (strcasecmp(name, "robust") == 0) return LATENCY_ROBUST;
    return LATENCY_INVALID;
}

static void apply_latency_profile(StreamConfig *config) {
    switch (config->latency) {
        case LATENCY_LOW:
            config->analyze_duration_us = 500000;
            config->probe_size_bytes = 1000000;
            config->linger_ms = 5000;
            config->keyframe_interval_ms = 1000;
            config->no_buffer = 1;
            break;
        case LATENCY_ROBUST:
            config->analyze_duration_us = 3000000;
            config->probe_size_bytes = 20000000;
            config->linger_ms = 10000;
            config->keyframe_interval_ms = 3000;
            config->no_buffer = 0;
            break;
        case LATENCY_BALANCED:
        default:
            config->analyze_duration_us = 1000000;
            config->probe_size_bytes = 5000000;
            config->linger_ms = 5000;
            config->keyframe_interval_ms = 2000;
            config->no_buffer = 0;
            break;
    }
}

int stream_config_finalize(StreamConfig *config,
                           TranscodeContainer path_container,
                           TranscodeContainer query_container) {
    if (!config || config->backend == BACKEND_INVALID ||
        config->codec == CODEC_INVALID || config->latency == LATENCY_INVALID) {
        return 0;
    }
    if (path_container != OUTPUT_INVALID && query_container != OUTPUT_INVALID &&
        path_container != query_container) {
        return 0;
    }

    TranscodeContainer requested = query_container != OUTPUT_INVALID
        ? query_container : path_container;
    if (requested == OUTPUT_INVALID) {
        requested = config->codec == CODEC_AV1 && config->backend == BACKEND_SOFTWARE
            ? OUTPUT_MATROSKA : OUTPUT_MPEGTS;
    }

    /* Preserve the established software-AV1 Matroska contract. */
    if (config->codec == CODEC_AV1 && config->backend == BACKEND_SOFTWARE &&
        requested != OUTPUT_MATROSKA) {
        return 0;
    }

    config->container = requested;

    /* These options do not affect passthrough output. Canonicalize them so
     * equivalent compatibility URLs converge on the same producer. */
    if (config->codec == CODEC_COPY) {
        config->backend = BACKEND_SOFTWARE;
        config->bitrate_kbps = 0;
        config->audio_channels = 2;
    }

    apply_latency_profile(config);

    /* Joining a header-oriented Matroska stream in progress is unsafe. */
    if (config->container != OUTPUT_MPEGTS) config->linger_ms = 0;
    return 1;
}

int stream_config_parse_channel_path(const char *path,
                                     char *channel,
                                     size_t channel_size,
                                     TranscodeContainer *path_container) {
    if (!path || !*path || !channel || channel_size == 0 || !path_container) return 0;

    size_t length = strlen(path);
    size_t channel_length = length;
    *path_container = OUTPUT_INVALID;
    if (length > 3 && strcasecmp(path + length - 3, ".ts") == 0) {
        channel_length -= 3;
        *path_container = OUTPUT_MPEGTS;
    } else if (length > 4 && strcasecmp(path + length - 4, ".mkv") == 0) {
        channel_length -= 4;
        *path_container = OUTPUT_MATROSKA;
    }

    if (channel_length == 0 || channel_length >= channel_size) return 0;
    memcpy(channel, path, channel_length);
    channel[channel_length] = '\0';
    return 1;
}

const char *stream_config_extension(const StreamConfig *config) {
    return config && config->container == OUTPUT_MATROSKA ? ".mkv" : ".ts";
}

const char *stream_config_mime_type(const StreamConfig *config) {
    return config && config->container == OUTPUT_MATROSKA
        ? "video/x-matroska" : "video/mp2t";
}

const char *stream_latency_name(StreamLatency latency) {
    switch (latency) {
        case LATENCY_LOW: return "low";
        case LATENCY_ROBUST: return "robust";
        case LATENCY_BALANCED: return "balanced";
        default: return "invalid";
    }
}

int stream_config_equal(const StreamConfig *left, const StreamConfig *right) {
    if (!left || !right) return 0;
    return strcmp(left->channel_num, right->channel_num) == 0 &&
        left->backend == right->backend &&
        left->codec == right->codec &&
        left->container == right->container &&
        left->latency == right->latency &&
        left->bitrate_kbps == right->bitrate_kbps &&
        left->audio_channels == right->audio_channels;
}

int stream_config_shareable(const StreamConfig *config) {
    return config && config->container == OUTPUT_MPEGTS;
}
