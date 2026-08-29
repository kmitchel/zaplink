#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stream_config.h"
#include "transcode.h"

int g_verbose = 0;

/* Helper to check if a specific string exists in the argument list */
static int has_arg(char **args, const char *target) {
    if (!args || !target) return 0;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], target) == 0) return 1;
    }
    return 0;
}

/* Helper to get argument immediately following a flag */
static const char *get_arg_val(char **args, const char *flag) {
    if (!args || !flag) return NULL;
    for (int i = 0; args[i] != NULL && args[i+1] != NULL; i++) {
        if (strcmp(args[i], flag) == 0) return args[i+1];
    }
    return NULL;
}

static void test_copy_stream(void) {
    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "15.1");
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));

    char *args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&config, args, &err);
    assert(err == 0);
    assert(has_arg(args, "ffmpeg"));
    assert(has_arg(args, "copy"));
    assert(has_arg(args, "mpegts"));
}

static void test_software_h264(void) {
    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "15.1");
    config.codec = CODEC_H264;
    config.backend = BACKEND_SOFTWARE;
    config.bitrate_kbps = 4500;
    config.audio_channels = 2;
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));

    char *args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&config, args, &err);
    assert(err == 0);
    assert(has_arg(args, "libx264"));
    assert(has_arg(args, "ultrafast"));
    assert(has_arg(args, "zerolatency"));
    assert(has_arg(args, "yadif=0:-1:1,format=yuv420p"));
    assert(strcmp(get_arg_val(args, "-b:v"), "4500k") == 0);
    assert(strcmp(get_arg_val(args, "-ac"), "2") == 0);
}

static void test_software_hevc(void) {
    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "15.1");
    config.codec = CODEC_HEVC;
    config.backend = BACKEND_SOFTWARE;
    config.audio_channels = 6;
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));

    char *args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&config, args, &err);
    assert(err == 0);
    assert(has_arg(args, "libx265"));
    assert(has_arg(args, "ultrafast"));
    assert(strcmp(get_arg_val(args, "-ac"), "6") == 0);
}

static void test_software_av1_matroska(void) {
    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "15.1");
    config.codec = CODEC_AV1;
    config.backend = BACKEND_SOFTWARE;
    assert(stream_config_finalize(&config, OUTPUT_INVALID, OUTPUT_INVALID));
    assert(config.container == OUTPUT_MATROSKA);

    char *args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&config, args, &err);
    assert(err == 0);
    assert(has_arg(args, "libsvtav1"));
    assert(has_arg(args, "matroska"));
}

static void test_qsv_backend(void) {
    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "15.1");
    config.codec = CODEC_H264;
    config.backend = BACKEND_QSV;
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));

    char *args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&config, args, &err);
    assert(err == 0);
    assert(has_arg(args, "h264_qsv"));
    assert(has_arg(args, "vpp_qsv=deinterlace=2"));
    assert(strcmp(get_arg_val(args, "-hwaccel"), "qsv") == 0);
}

static void test_nvenc_backend(void) {
    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "15.1");
    config.codec = CODEC_HEVC;
    config.backend = BACKEND_NVENC;
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));

    char *args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&config, args, &err);
    assert(err == 0);
    assert(has_arg(args, "hevc_nvenc"));
    assert(has_arg(args, "yadif_cuda=0:-1:1"));
    assert(strcmp(get_arg_val(args, "-hwaccel"), "cuda") == 0);
}

static void test_vaapi_backend(void) {
    StreamConfig config;
    stream_config_init(&config);
    strcpy(config.channel_num, "15.1");
    config.codec = CODEC_H264;
    config.backend = BACKEND_VAAPI;
    assert(stream_config_finalize(&config, OUTPUT_MPEGTS, OUTPUT_INVALID));

    char *args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&config, args, &err);
    assert(err == 0);
    assert(has_arg(args, "h264_vaapi"));
    assert(has_arg(args, "deinterlace_vaapi"));
    assert(strcmp(get_arg_val(args, "-hwaccel"), "vaapi") == 0);
}

static void test_latency_profiles(void) {
    StreamConfig low_cfg;
    stream_config_init(&low_cfg);
    strcpy(low_cfg.channel_num, "15.1");
    low_cfg.latency = LATENCY_LOW;
    assert(stream_config_finalize(&low_cfg, OUTPUT_MPEGTS, OUTPUT_INVALID));

    char *low_args[128] = {0};
    int err = 0;
    build_ffmpeg_arguments(&low_cfg, low_args, &err);
    assert(err == 0);
    assert(strcmp(get_arg_val(low_args, "-analyzeduration"), "500000") == 0);
    assert(strcmp(get_arg_val(low_args, "-probesize"), "1000000") == 0);
    assert(has_arg(low_args, "+genpts+discardcorrupt+nobuffer"));
}

int main(void) {
    test_copy_stream();
    test_software_h264();
    test_software_hevc();
    test_software_av1_matroska();
    test_qsv_backend();
    test_nvenc_backend();
    test_vaapi_backend();
    test_latency_profiles();

    puts("transcode argument builder tests: OK");
    return 0;
}
