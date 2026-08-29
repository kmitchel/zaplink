#include <assert.h>
#include <string.h>

#include "stream_config.h"

int main(void) {
    char channel[64];
    TranscodeContainer path_container;

    assert(stream_config_parse_channel_path("21.2", channel, sizeof(channel),
                                            &path_container));
    assert(strcmp(channel, "21.2") == 0);
    assert(path_container == OUTPUT_INVALID);

    assert(stream_config_parse_channel_path("21.2.ts", channel, sizeof(channel),
                                            &path_container));
    assert(strcmp(channel, "21.2") == 0);
    assert(path_container == OUTPUT_MPEGTS);

    assert(stream_config_parse_channel_path("15.1-581000000-3.mkv", channel,
                                            sizeof(channel), &path_container));
    assert(strcmp(channel, "15.1-581000000-3") == 0);
    assert(path_container == OUTPUT_MATROSKA);

    StreamConfig balanced;
    stream_config_init(&balanced);
    strcpy(balanced.channel_num, "21.2");
    assert(stream_config_finalize(&balanced, OUTPUT_INVALID, OUTPUT_INVALID));
    assert(balanced.container == OUTPUT_MPEGTS);
    assert(balanced.analyze_duration_us == 1000000);
    assert(balanced.probe_size_bytes == 5000000);
    assert(balanced.linger_ms == 5000);
    assert(strcmp(stream_config_extension(&balanced), ".ts") == 0);
    assert(strcmp(stream_config_mime_type(&balanced), "video/mp2t") == 0);

    StreamConfig low = balanced;
    low.latency = LATENCY_LOW;
    assert(stream_config_finalize(&low, OUTPUT_MPEGTS, OUTPUT_INVALID));
    assert(low.analyze_duration_us == 500000);
    assert(low.probe_size_bytes == 1000000);
    assert(low.keyframe_interval_ms == 1000);
    assert(low.no_buffer == 1);

    StreamConfig av1;
    stream_config_init(&av1);
    av1.codec = CODEC_AV1;
    assert(stream_config_finalize(&av1, OUTPUT_INVALID, OUTPUT_INVALID));
    assert(av1.container == OUTPUT_MATROSKA);
    assert(av1.linger_ms == 0);
    assert(strcmp(stream_config_extension(&av1), ".mkv") == 0);
    assert(!stream_config_shareable(&av1));

    stream_config_init(&av1);
    av1.codec = CODEC_AV1;
    assert(!stream_config_finalize(&av1, OUTPUT_MPEGTS, OUTPUT_INVALID));

    StreamConfig first;
    StreamConfig second;
    stream_config_init(&first);
    stream_config_init(&second);
    strcpy(first.channel_num, "38.7");
    strcpy(second.channel_num, "38.7");
    first.backend = second.backend = BACKEND_QSV;
    first.codec = second.codec = CODEC_H264;
    first.bitrate_kbps = second.bitrate_kbps = 6000;
    assert(stream_config_finalize(&first, OUTPUT_INVALID, OUTPUT_MPEGTS));
    assert(stream_config_finalize(&second, OUTPUT_MPEGTS, OUTPUT_INVALID));
    assert(stream_config_equal(&first, &second));
    second.audio_channels = 6;
    assert(!stream_config_equal(&first, &second));

    StreamConfig copy_first;
    StreamConfig copy_second;
    stream_config_init(&copy_first);
    stream_config_init(&copy_second);
    strcpy(copy_first.channel_num, "21.2");
    strcpy(copy_second.channel_num, "21.2");
    copy_second.backend = BACKEND_VAAPI;
    copy_second.bitrate_kbps = 9000;
    copy_second.audio_channels = 6;
    assert(stream_config_finalize(&copy_first, OUTPUT_INVALID, OUTPUT_INVALID));
    assert(stream_config_finalize(&copy_second, OUTPUT_MPEGTS, OUTPUT_INVALID));
    assert(stream_config_equal(&copy_first, &copy_second));

    assert(parse_backend("VaApI") == BACKEND_VAAPI);
    assert(parse_codec("h265") == CODEC_HEVC);
    assert(parse_container("mpegts") == OUTPUT_MPEGTS);
    assert(parse_latency("robust") == LATENCY_ROBUST);
    assert(parse_latency("fastest") == LATENCY_INVALID);
    return 0;
}
