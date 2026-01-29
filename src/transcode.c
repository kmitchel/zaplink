#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include "transcode.h"
#include "log.h"
#include "config.h"
#include "tuner.h"
#include "channels.h"

TranscodeBackend parse_backend(const char *name) {
    if (!name) return BACKEND_SOFTWARE;
    if (strcasecmp(name, "qsv") == 0) return BACKEND_QSV;
    if (strcasecmp(name, "nvenc") == 0) return BACKEND_NVENC;
    if (strcasecmp(name, "vaapi") == 0) return BACKEND_VAAPI;
    if (strcasecmp(name, "software") == 0) return BACKEND_SOFTWARE;
    return BACKEND_INVALID;
}

TranscodeCodec parse_codec(const char *name) {
    if (!name) return CODEC_H264;
    if (strcasecmp(name, "h264") == 0) return CODEC_H264;
    if (strcasecmp(name, "hevc") == 0 || strcasecmp(name, "h265") == 0) return CODEC_HEVC;
    if (strcasecmp(name, "av1") == 0) return CODEC_AV1;
    if (strcasecmp(name, "copy") == 0) return CODEC_COPY;
    return CODEC_INVALID;
}

/**
 * Validate channel number format to prevent shell injection.
 * Only allows digits, dots, and hyphens (e.g., "15.1", "21-1", "33.2").
 * Returns 1 if valid, 0 if invalid.
 */
static int validate_channel_num(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit(*p) && *p != '.' && *p != '-') {
            return 0;
        }
    }
    return 1;
}

/**
 * Write all bytes to socket, handling partial writes and EINTR.
 * With SO_SNDTIMEO set on the socket, write() will timeout after the configured duration.
 * Returns 1 on success, 0 on error/timeout (client disconnected or slow).
 */
static int write_all(int fd, const char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0; // Error, timeout, or disconnect
        }
        written += n;
    }
    return 1;
}

void handle_unified_stream(int sockfd, StreamConfig *config) {
    // SECURITY: Validate channel_num to prevent shell injection
    if (!validate_channel_num(config->channel_num)) {
        LOG_WARN("TRANSCODE", "Invalid channel number format: %s", config->channel_num);
        return;
    }

    Channel *c = find_channel_by_number(config->channel_num);
    if (!c) {
        LOG_WARN("TRANSCODE", "Channel not found: %s", config->channel_num);
        return;
    }

    Tuner *t = acquire_tuner(USER_STREAM);
    if (!t) {
        LOG_WARN("TRANSCODE", "No tuner available for stream");
        return;
    }

    char cmd[2048];
    char v_encoder[128] = "libx264";
    char v_hw_args[256] = "";
    char v_filter[128] = "";
    int is_software_av1 = 0;

    // Configure Backend
    if (config->codec != CODEC_COPY) {
        switch (config->backend) {
            case BACKEND_QSV:
                snprintf(v_hw_args, sizeof(v_hw_args), "-hwaccel qsv -hwaccel_output_format qsv -init_hw_device qsv=qsv:hw -filter_hw_device qsv");
                if (config->codec == CODEC_H264) strcpy(v_encoder, "h264_qsv -look_ahead 0");
                else if (config->codec == CODEC_HEVC) strcpy(v_encoder, "hevc_qsv -look_ahead 0");
                else if (config->codec == CODEC_AV1) strcpy(v_encoder, "av1_qsv");
                strcpy(v_filter, "-vf \"vpp_qsv=deinterlace=2\"");
                break;
            case BACKEND_NVENC:
                snprintf(v_hw_args, sizeof(v_hw_args), "-hwaccel cuda -hwaccel_output_format cuda");
                if (config->codec == CODEC_H264) strcpy(v_encoder, "h264_nvenc");
                else if (config->codec == CODEC_HEVC) strcpy(v_encoder, "hevc_nvenc");
                else if (config->codec == CODEC_AV1) strcpy(v_encoder, "av1_nvenc");
                strcpy(v_filter, "-vf \"yadif_cuda\"");
                break;
            case BACKEND_VAAPI:
                snprintf(v_hw_args, sizeof(v_hw_args), "-hwaccel vaapi -hwaccel_output_format vaapi -hwaccel_device /dev/dri/renderD128");
                if (config->codec == CODEC_H264) strcpy(v_encoder, "h264_vaapi");
                else if (config->codec == CODEC_HEVC) strcpy(v_encoder, "hevc_vaapi");
                else if (config->codec == CODEC_AV1) strcpy(v_encoder, "av1_vaapi");
                strcpy(v_filter, "-vf \"deinterlace_vaapi\"");
                break;
            default: // SOFTWARE
                if (config->codec == CODEC_HEVC) strcpy(v_encoder, "libx265");
                else if (config->codec == CODEC_AV1) { 
                    strcpy(v_encoder, "libsvtav1 -preset 10 -svtav1-params row-mt=1"); 
                    is_software_av1 = 1; 
                }
                else strcpy(v_encoder, "libx264");
                strcpy(v_filter, "-vf \"yadif,format=yuv420p\"");
                break;
        }
    } else {
        strcpy(v_encoder, "copy");
        v_filter[0] = '\0'; // Ensure no filter for copy mode
    }

    char adapter_id[16];
    snprintf(adapter_id, sizeof(adapter_id), "%d", t->id);

    // Build FFmpeg command for piped output
    int bitrate = config->bitrate_kbps;
    
    // AV1 in software needs Matroska; hardware AV1 encoders can use MPEG-TS
    const char *out_fmt = (is_software_av1) ? "matroska" : "mpegts";
    
    // Rate control: if bitrate > 0, use ABR; otherwise let encoder use default (typically CRF/quality)
    char v_rate_control[256] = "";
    if (bitrate > 0) {
        if (is_software_av1) {
            // SVT-AV1 doesn't support -maxrate/-bufsize in ABR mode
            snprintf(v_rate_control, sizeof(v_rate_control), "-b:v %dk", bitrate);
        } else if (config->codec != CODEC_COPY) {
            snprintf(v_rate_control, sizeof(v_rate_control), "-b:v %dk -maxrate %dk -bufsize %dk", 
                     bitrate, bitrate * 2, bitrate * 4);
        }
    }

    // NOTE: c->number is used here, not config->channel_num. 
    // c->number comes from channels.conf which is trusted local data.
    snprintf(cmd, sizeof(cmd),
        "dvbv5-zap -c %s -a %s -o - %s | ffmpeg %s -i - %s -c:v %s %s -c:a aac -ac %d -f %s -",
        channels_conf_path, adapter_id, c->number, v_hw_args, v_filter, v_encoder, 
        v_rate_control, config->audio_channels, out_fmt);

    LOG_INFO("TRANSCODE", "Starting stream: %s", config->channel_num);
    LOG_DEBUG("TRANSCODE", "Command: %s", cmd);
    
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        LOG_ERROR("TRANSCODE", "Failed to create pipe");
        release_tuner(t);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("TRANSCODE", "Fork failed");
        close(pipefds[0]);
        close(pipefds[1]);
        release_tuner(t);
        return;
    }
    
    if (pid == 0) {
        // Child: Setup process group and execute pipeline
        setpgid(0, 0);
        close(pipefds[0]);
        dup2(pipefds[1], STDOUT_FILENO);
        close(pipefds[1]);
        
        // Suppress stderr unless verbose mode is enabled
        if (!g_verbose) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    // Parent
    close(pipefds[1]);
    
    char buffer[16384];
    ssize_t n;
    while ((n = read(pipefds[0], buffer, sizeof(buffer))) > 0) {
        if (!write_all(sockfd, buffer, n)) {
            LOG_INFO("TRANSCODE", "Client disconnected, killing stream group %d", pid);
            break;
        }
    }

    // Kill the entire process group (negative PID)
    kill(-pid, SIGTERM);
    
    // Cleanup
    close(pipefds[0]);
    int status;
    waitpid(pid, &status, 0);
    release_tuner(t);
    LOG_INFO("TRANSCODE", "Stream ended for %s", config->channel_num);
}
