#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
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

enum QueryField {
    QUERY_BACKEND,
    QUERY_CODEC,
    QUERY_BITRATE,
    QUERY_AUDIO,
    QUERY_LATENCY,
    QUERY_CONTAINER,
    QUERY_FIELD_COUNT
};

static const char *const query_names[QUERY_FIELD_COUNT] = {
    "backend", "codec", "bitrate", "audio", "latency", "container"
};

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    value = (unsigned char)tolower(value);
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static int decode_component(const char *source, size_t source_length,
                            char *destination, size_t destination_size) {
    if (source_length == 0 || source_length >= destination_size) return 0;
    size_t output = 0;
    for (size_t input = 0; input < source_length; input++) {
        unsigned char value = (unsigned char)source[input];
        if (value == '%') {
            if (input + 2 >= source_length) return 0;
            int high = hex_value((unsigned char)source[input + 1]);
            int low = hex_value((unsigned char)source[input + 2]);
            if (high < 0 || low < 0) return 0;
            value = (unsigned char)((high << 4) | low);
            input += 2;
        } else if (value == '+') {
            value = ' ';
        }
        if (value == 0 || value < 0x20 || value == 0x7f) return 0;
        destination[output++] = (char)value;
    }
    destination[output] = '\0';
    return output > 0;
}

static int query_field(const char *name) {
    for (int field = 0; field < QUERY_FIELD_COUNT; field++) {
        if (strcmp(name, query_names[field]) == 0) return field;
    }
    return -1;
}

static int parse_bounded_int(const char *text, int minimum, int maximum,
                             int *value) {
    if (!text || !*text || !value) return 0;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < minimum ||
        parsed > maximum) return 0;
    *value = (int)parsed;
    return 1;
}

static int parse_query(const char *query,
                       char values[QUERY_FIELD_COUNT][64],
                       unsigned int *present,
                       char *error, size_t error_size) {
    *present = 0;
    if (!query || !*query) return 1;

    const char *parameter = query;
    while (*parameter) {
        const char *end = strchr(parameter, '&');
        if (!end) end = parameter + strlen(parameter);
        const char *equals = memchr(parameter, '=', (size_t)(end - parameter));
        char name[32];
        if (!equals || equals == parameter || equals + 1 == end ||
            !decode_component(parameter, (size_t)(equals - parameter),
                              name, sizeof(name))) {
            snprintf(error, error_size, "Malformed or empty query parameter");
            return 0;
        }
        int field = query_field(name);
        if (field < 0) {
            snprintf(error, error_size, "Unknown query parameter: %s", name);
            return 0;
        }
        unsigned int bit = 1U << (unsigned int)field;
        if (*present & bit) {
            snprintf(error, error_size, "Duplicate query parameter: %s", name);
            return 0;
        }
        if (!decode_component(equals + 1, (size_t)(end - equals - 1),
                              values[field], sizeof(values[field]))) {
            snprintf(error, error_size, "Invalid value for query parameter: %s", name);
            return 0;
        }
        *present |= bit;
        parameter = *end ? end + 1 : end;
        if (!*parameter && *end == '&') {
            snprintf(error, error_size, "Malformed or empty query parameter");
            return 0;
        }
    }
    return 1;
}

int stream_config_parse_request(const char *channel_path,
                                const char *query,
                                StreamConfig *config,
                                char *error,
                                size_t error_size) {
    if (!config || !error || error_size == 0) return 0;
    error[0] = '\0';
    stream_config_init(config);

    TranscodeContainer path_container = OUTPUT_INVALID;
    if (channel_path &&
        !stream_config_parse_channel_path(channel_path, config->channel_num,
                                          sizeof(config->channel_num),
                                          &path_container)) {
        snprintf(error, error_size, "Invalid channel identifier");
        return 0;
    }

    char values[QUERY_FIELD_COUNT][64] = {{0}};
    unsigned int present = 0;
    if (!parse_query(query, values, &present, error, error_size)) return 0;

#define HAS(field) (present & (1U << (unsigned int)(field)))
    config->backend = HAS(QUERY_BACKEND)
        ? parse_backend(values[QUERY_BACKEND]) : BACKEND_SOFTWARE;
    config->codec = HAS(QUERY_CODEC)
        ? parse_codec(values[QUERY_CODEC]) : CODEC_COPY;
    config->latency = HAS(QUERY_LATENCY)
        ? parse_latency(values[QUERY_LATENCY]) : LATENCY_BALANCED;
    TranscodeContainer query_container = HAS(QUERY_CONTAINER)
        ? parse_container(values[QUERY_CONTAINER]) : OUTPUT_INVALID;
    if (config->backend == BACKEND_INVALID || config->codec == CODEC_INVALID ||
        config->latency == LATENCY_INVALID ||
        (HAS(QUERY_CONTAINER) && query_container == OUTPUT_INVALID)) {
        snprintf(error, error_size,
                 "Invalid backend, codec, container, or latency profile");
        return 0;
    }

    if (HAS(QUERY_BITRATE) &&
        !parse_bounded_int(values[QUERY_BITRATE], 1, MAX_BITRATE_KBPS,
                           &config->bitrate_kbps)) {
        snprintf(error, error_size, "Invalid bitrate");
        return 0;
    }
    if (HAS(QUERY_AUDIO)) {
        const char *audio = values[QUERY_AUDIO];
        if (strcmp(audio, "6") == 0 || strcmp(audio, "5.1") == 0 ||
            strcmp(audio, "51") == 0) {
            config->audio_channels = 6;
        } else if (!parse_bounded_int(audio, 1, 8,
                                      &config->audio_channels)) {
            snprintf(error, error_size, "Invalid audio channel count");
            return 0;
        }
    }

    if (!stream_config_finalize(config, path_container, query_container)) {
        snprintf(error, error_size, "Container conflicts with the URL or codec");
        return 0;
    }
#undef HAS
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
