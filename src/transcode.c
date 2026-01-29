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
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
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

void handle_unified_stream(int sockfd, StreamConfig *config, const char *http_header) {
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

    char adapter_id[16];
    snprintf(adapter_id, sizeof(adapter_id), "%d", t->id);

    char cmd[2048];
    
    // Passthrough mode: no transcoding, pipe dvbv5-zap directly to client
    if (config->codec == CODEC_COPY) {
        snprintf(cmd, sizeof(cmd), "dvbv5-zap -c %s -a %s -o - %s",
            channels_conf_path, adapter_id, c->number);
        LOG_INFO("TRANSCODE", "Starting passthrough stream: %s", config->channel_num);
    } else {
        // Transcoding mode: use ffmpeg
        char v_encoder[256] = "libx264 -preset ultrafast -tune zerolatency";
        char v_hw_args[256] = "";
        char v_filter[128] = "";
        int is_software_av1 = 0;

        switch (config->backend) {
            case BACKEND_QSV:
                snprintf(v_hw_args, sizeof(v_hw_args), "-hwaccel qsv -hwaccel_output_format qsv -init_hw_device qsv=qsv:hw -filter_hw_device qsv");
                if (config->codec == CODEC_H264) strcpy(v_encoder, "h264_qsv -look_ahead 0 -async_depth 1");
                else if (config->codec == CODEC_HEVC) strcpy(v_encoder, "hevc_qsv -look_ahead 0 -async_depth 1");
                else if (config->codec == CODEC_AV1) strcpy(v_encoder, "av1_qsv -async_depth 1");
                strcpy(v_filter, "-vf \"vpp_qsv=deinterlace=2\"");
                break;
            case BACKEND_NVENC:
                snprintf(v_hw_args, sizeof(v_hw_args), "-hwaccel cuda -hwaccel_output_format cuda");
                if (config->codec == CODEC_H264) strcpy(v_encoder, "h264_nvenc -preset p1 -tune ll -zerolatency 1");
                else if (config->codec == CODEC_HEVC) strcpy(v_encoder, "hevc_nvenc -preset p1 -tune ll -zerolatency 1");
                else if (config->codec == CODEC_AV1) strcpy(v_encoder, "av1_nvenc -preset p1 -tune ll");
                strcpy(v_filter, "-vf \"yadif_cuda\"");
                break;
            case BACKEND_VAAPI:
                snprintf(v_hw_args, sizeof(v_hw_args), "-hwaccel vaapi -hwaccel_output_format vaapi -hwaccel_device /dev/dri/renderD128");
                if (config->codec == CODEC_H264) strcpy(v_encoder, "h264_vaapi -compression_level 0");
                else if (config->codec == CODEC_HEVC) strcpy(v_encoder, "hevc_vaapi -compression_level 0");
                else if (config->codec == CODEC_AV1) strcpy(v_encoder, "av1_vaapi");
                strcpy(v_filter, "-vf \"deinterlace_vaapi\"");
                break;
            default: // SOFTWARE
                if (config->codec == CODEC_HEVC) strcpy(v_encoder, "libx265 -preset ultrafast");
                else if (config->codec == CODEC_AV1) { 
                    strcpy(v_encoder, "libsvtav1 -preset 12"); 
                    is_software_av1 = 1; 
                }
                else strcpy(v_encoder, "libx264 -preset ultrafast -tune zerolatency");
                strcpy(v_filter, "-vf \"yadif,format=yuv420p\"");
                break;
        }

        int bitrate = config->bitrate_kbps;
        
        // AV1 in software needs Matroska; hardware AV1 encoders can use MPEG-TS
        const char *out_fmt = (is_software_av1) ? "matroska" : "mpegts";
        
        // MPEG-TS options for live streaming: resend headers, frequent PSI tables
        const char *mux_opts = (is_software_av1) ? "" : "-mpegts_flags +resend_headers -pat_period 0.1 -sdt_period 0.5";
        
        // Rate control: if bitrate > 0, use ABR; otherwise let encoder use default (typically CRF/quality)
        char v_rate_control[256] = "";
        if (bitrate > 0) {
            if (is_software_av1) {
                // SVT-AV1 doesn't support -maxrate/-bufsize in ABR mode
                snprintf(v_rate_control, sizeof(v_rate_control), "-b:v %dk", bitrate);
            } else {
                snprintf(v_rate_control, sizeof(v_rate_control), "-b:v %dk -maxrate %dk -bufsize %dk", 
                         bitrate, bitrate * 2, bitrate * 4);
            }
        }

        // NOTE: c->number is used here, not config->channel_num. 
        // c->number comes from channels.conf which is trusted local data.
        // FFmpeg input flags for live streaming:
        //   -fflags +genpts+discardcorrupt: Generate PTS, discard corrupt frames
        //   -analyzeduration 1M -probesize 1M: Faster stream detection (1 second)
        //   -thread_queue_size 512: Larger input buffer for bursty input
        snprintf(cmd, sizeof(cmd),
            "dvbv5-zap -c %s -a %s -o - %s | ffmpeg %s -fflags +genpts+discardcorrupt "
            "-analyzeduration 1000000 -probesize 1000000 -thread_queue_size 512 -i - "
            "%s -c:v %s %s -g 60 -c:a aac -ac %d -f %s %s -",
            channels_conf_path, adapter_id, c->number, v_hw_args, v_filter, v_encoder, 
            v_rate_control, config->audio_channels, out_fmt, mux_opts);

        LOG_INFO("TRANSCODE", "Starting stream: %s", config->channel_num);
    }

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
    
    // Enable TCP keepalive to detect dead connections
    int keepalive = 1;
    int keepidle = 10;   // Start probing after 10 seconds idle
    int keepintvl = 5;   // Probe every 5 seconds
    int keepcnt = 3;     // Give up after 3 failed probes
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
    
    // TCP_NODELAY: Disable Nagle's algorithm for lower latency streaming
    int nodelay = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    
    // Larger send buffer for smoother streaming (256KB)
    int sndbuf = 256 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    // Set socket send timeout (shorter for streaming)
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    char buffer[16384];
    ssize_t n;
    int header_sent = 0;
    int client_alive = 1;
    
    // Use poll to monitor both pipe and socket for errors
    struct pollfd fds[2];
    fds[0].fd = pipefds[0];  // Pipe from dvbv5-zap/ffmpeg
    fds[0].events = POLLIN;
    fds[1].fd = sockfd;      // Client socket
    fds[1].events = 0;       // Only care about errors/hangup (always reported)
    
    while (client_alive) {
        // Poll with 5 second timeout to periodically check socket health
        int ret = poll(fds, 2, 5000);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("TRANSCODE", "poll() error: %s", strerror(errno));
            break;
        }
        
        // Check for socket errors/hangup (client disconnected)
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_INFO("TRANSCODE", "Client socket error/hangup detected, killing stream group %d", pid);
            break;
        }
        
        // Check for data from pipe
        if (fds[0].revents & POLLIN) {
            n = read(pipefds[0], buffer, sizeof(buffer));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;  // EOF or error from pipe
            }
            
            // Send HTTP header only after we have data ready
            if (!header_sent) {
                if (!write_all(sockfd, http_header, strlen(http_header))) {
                    LOG_INFO("TRANSCODE", "Client disconnected before stream started");
                    break;
                }
                header_sent = 1;
                LOG_DEBUG("TRANSCODE", "Stream data ready, sent HTTP header");
            }
            
            if (!write_all(sockfd, buffer, n)) {
                LOG_INFO("TRANSCODE", "Client disconnected, killing stream group %d", pid);
                break;
            }
        }
        
        // Check for pipe errors (process died)
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            // Drain any remaining data before exiting
            while ((n = read(pipefds[0], buffer, sizeof(buffer))) > 0) {
                if (!write_all(sockfd, buffer, n)) break;
            }
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
