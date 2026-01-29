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

// Helper to append args to argv with sticky error handling
static void add_arg(char **argv, int *argc, const char *arg, int *err) {
    if (*err) return; // Propagate error (sticky)

    if (*argc < 127) {
        argv[*argc] = (char *)arg;
        argv[*argc + 1] = NULL;
        (*argc)++;
        return;
    }
    
    // Overflow detected
    LOG_ERROR("TRANSCODE", "Argv limit exceeded (128), dropping argument: %s", arg);
    *err = 1;
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

    LOG_INFO("TRANSCODE", "Starting stream: %s (Codec: %d, Backend: %d)", 
             config->channel_num, config->codec, config->backend);
    
    // Create pipe between dvbv5-zap and ffmpeg
    int zap_pipe[2];
    if (pipe(zap_pipe) < 0) {
        LOG_ERROR("TRANSCODE", "Failed to create zap pipe");
        release_tuner(t);
        return;
    }
    
    // Create pipe for output to client (ffmpeg -> parent -> client)
    int pipefds[2];
    if (pipe(pipefds) < 0) {
        LOG_ERROR("TRANSCODE", "Failed to create output pipe");
        close(zap_pipe[0]);
        close(zap_pipe[1]);
        release_tuner(t);
        return;
    }

    // Fork main child process (group leader)
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("TRANSCODE", "Fork failed");
        close(zap_pipe[0]);
        close(zap_pipe[1]);
        close(pipefds[0]);
        close(pipefds[1]);
        release_tuner(t);
        return;
    }
    
    if (pid == 0) {
        // Child: Setup process group
        setpgid(0, 0);
        
        // Suppress stderr unless verbose mode is enabled
        if (!g_verbose) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        // Fork dvbv5-zap
        pid_t zap_pid = fork();
        if (zap_pid == 0) {
            // dvbv5-zap process
            close(pipefds[0]); // Unused
            close(pipefds[1]); // Unused
            close(zap_pipe[0]); // Close read end
            
            // Redirect stdout to pipe
            dup2(zap_pipe[1], STDOUT_FILENO);
            close(zap_pipe[1]);
            
            // Direct exec without shell
            execlp("dvbv5-zap", "dvbv5-zap", 
                   "-c", channels_conf_path,
                   "-a", adapter_id,
                   "-o", "-",
                   c->number, NULL);
            _exit(1);
        }
        
        // Fork ffmpeg
        pid_t ffmpeg_pid = fork();
        if (ffmpeg_pid == 0) {
            // ffmpeg process
            close(pipefds[0]); // Unused
            
            // Redirect stdin from zap pipe
            dup2(zap_pipe[0], STDIN_FILENO);
            close(zap_pipe[0]);
            close(zap_pipe[1]); // Close write end
            
            // Redirect stdout to main pipe
            dup2(pipefds[1], STDOUT_FILENO);
            close(pipefds[1]);

            // Construct argv for execvp directly
            char *args[128];
            int n = 0;
            int err = 0; // Sticky error flag
            
            add_arg(args, &n, "ffmpeg", &err);
            
            // Input options (common)
            add_arg(args, &n, "-fflags", &err); add_arg(args, &n, "+genpts+discardcorrupt", &err);
            add_arg(args, &n, "-analyzeduration", &err); add_arg(args, &n, "1000000", &err);
            add_arg(args, &n, "-probesize", &err); add_arg(args, &n, "5000000", &err);
            add_arg(args, &n, "-thread_queue_size", &err); add_arg(args, &n, "512", &err);
            
            // Input file (stdin)
            add_arg(args, &n, "-f", &err); add_arg(args, &n, "mpegts", &err);
            add_arg(args, &n, "-i", &err); add_arg(args, &n, "-", &err);
            
            // Codec Configuration
            char ac_str[8]; // Buffer for audio channels
            char bitrate_str[16]; // Buffer for bitrate
            
            if (config->codec == CODEC_COPY) {
                // Passthrough Mode
                add_arg(args, &n, "-c", &err); add_arg(args, &n, "copy", &err);
                
                // Muxer flags for clean TS output
                add_arg(args, &n, "-f", &err); add_arg(args, &n, "mpegts", &err);
                add_arg(args, &n, "-mpegts_flags", &err); add_arg(args, &n, "+resend_headers", &err);
                add_arg(args, &n, "-pat_period", &err); add_arg(args, &n, "0.1", &err);
                add_arg(args, &n, "-sdt_period", &err); add_arg(args, &n, "0.5", &err);
                
            } else {
                // Transcoding Mode
                
                // Hardware Acceleration Flags
                switch (config->backend) {
                    case BACKEND_QSV:
                        add_arg(args, &n, "-hwaccel", &err); add_arg(args, &n, "qsv", &err);
                        add_arg(args, &n, "-hwaccel_output_format", &err); add_arg(args, &n, "qsv", &err);
                        add_arg(args, &n, "-init_hw_device", &err); add_arg(args, &n, "qsv=qsv:hw", &err);
                        add_arg(args, &n, "-filter_hw_device", &err); add_arg(args, &n, "qsv", &err);
                        break;
                    case BACKEND_NVENC:
                        add_arg(args, &n, "-hwaccel", &err); add_arg(args, &n, "cuda", &err);
                        add_arg(args, &n, "-hwaccel_output_format", &err); add_arg(args, &n, "cuda", &err);
                        break;
                    case BACKEND_VAAPI:
                        add_arg(args, &n, "-hwaccel", &err); add_arg(args, &n, "vaapi", &err);
                        add_arg(args, &n, "-hwaccel_output_format", &err); add_arg(args, &n, "vaapi", &err);
                        add_arg(args, &n, "-hwaccel_device", &err); add_arg(args, &n, "/dev/dri/renderD128", &err);
                        break;
                    default: break;
                }
                
                // Video Filters
                add_arg(args, &n, "-vf", &err);
                switch (config->backend) {
                    case BACKEND_QSV: 
                        // QSV Advanced deinterlacing (usually respects frame flags)
                        add_arg(args, &n, "vpp_qsv=deinterlace=2", &err); 
                        break;
                    case BACKEND_NVENC: 
                        // yadif_cuda: mode 0 (frame), parity -1 (auto), deint 1 (interlaced only)
                        add_arg(args, &n, "yadif_cuda=0:-1:1", &err); 
                        break;
                    case BACKEND_VAAPI: 
                        // VAAPI motion adaptive
                        add_arg(args, &n, "deinterlace_vaapi", &err); 
                        break;
                    default: 
                        // Software yadif: mode 0 (frame), parity -1 (auto), deint 1 (interlaced only)
                        add_arg(args, &n, "yadif=0:-1:1,format=yuv420p", &err); 
                        break;
                }
                
                // Video Encoder & Options
                add_arg(args, &n, "-c:v", &err);
                
                if (config->backend == BACKEND_QSV) {
                    if (config->codec == CODEC_H264) {
                        add_arg(args, &n, "h264_qsv", &err);
                        add_arg(args, &n, "-look_ahead", &err); add_arg(args, &n, "0", &err);
                        add_arg(args, &n, "-async_depth", &err); add_arg(args, &n, "1", &err);
                    } else if (config->codec == CODEC_HEVC) {
                         add_arg(args, &n, "hevc_qsv", &err);
                         add_arg(args, &n, "-look_ahead", &err); add_arg(args, &n, "0", &err);
                         add_arg(args, &n, "-async_depth", &err); add_arg(args, &n, "1", &err);
                    } else if (config->codec == CODEC_AV1) {
                         add_arg(args, &n, "av1_qsv", &err);
                         add_arg(args, &n, "-async_depth", &err); add_arg(args, &n, "1", &err);
                    }
                } else if (config->backend == BACKEND_NVENC) {
                    if (config->codec == CODEC_H264) add_arg(args, &n, "h264_nvenc", &err);
                    else if (config->codec == CODEC_HEVC) add_arg(args, &n, "hevc_nvenc", &err);
                    else if (config->codec == CODEC_AV1) add_arg(args, &n, "av1_nvenc", &err);
                    
                    add_arg(args, &n, "-preset", &err); add_arg(args, &n, "p1", &err);
                    add_arg(args, &n, "-tune", &err); add_arg(args, &n, "ll", &err);
                    if (config->codec != CODEC_AV1) { // AV1 nvenc might not support zerolatency flag in all versions
                         add_arg(args, &n, "-zerolatency", &err); add_arg(args, &n, "1", &err);
                    }
                } else if (config->backend == BACKEND_VAAPI) {
                    if (config->codec == CODEC_H264) add_arg(args, &n, "h264_vaapi", &err);
                    else if (config->codec == CODEC_HEVC) add_arg(args, &n, "hevc_vaapi", &err);
                    else if (config->codec == CODEC_AV1) add_arg(args, &n, "av1_vaapi", &err);
                    if (config->codec != CODEC_AV1) {
                        add_arg(args, &n, "-compression_level", &err); add_arg(args, &n, "0", &err);
                    }
                } else { // SOFTWARE
                    if (config->codec == CODEC_HEVC) {
                        add_arg(args, &n, "libx265", &err);
                        add_arg(args, &n, "-preset", &err); add_arg(args, &n, "ultrafast", &err);
                    } else if (config->codec == CODEC_AV1) {
                        add_arg(args, &n, "libsvtav1", &err);
                        add_arg(args, &n, "-preset", &err); add_arg(args, &n, "12", &err);
                    } else {
                        add_arg(args, &n, "libx264", &err);
                        add_arg(args, &n, "-preset", &err); add_arg(args, &n, "ultrafast", &err);
                        add_arg(args, &n, "-tune", &err); add_arg(args, &n, "zerolatency", &err);
                    }
                }

                // Rate Control
                int rate = config->bitrate_kbps;
                if (rate > 0) {
                     snprintf(bitrate_str, sizeof(bitrate_str), "%dk", rate);
                     add_arg(args, &n, "-b:v", &err); add_arg(args, &n, bitrate_str, &err);
                     
                     if (config->backend != BACKEND_SOFTWARE || config->codec != CODEC_AV1) {
                         // SVT-AV1 has different RC params, simple copy logic skips -maxrate for it
                         char maxrate_str[16];
                         snprintf(maxrate_str, sizeof(maxrate_str), "%dk", rate * 2);
                         add_arg(args, &n, "-maxrate", &err); add_arg(args, &n, maxrate_str, &err);
                         
                         char bufsize_str[16];
                         snprintf(bufsize_str, sizeof(bufsize_str), "%dk", rate * 4);
                         add_arg(args, &n, "-bufsize", &err); add_arg(args, &n, bufsize_str, &err);
                     }
                }

                // GOP Size
                add_arg(args, &n, "-g", &err); add_arg(args, &n, "60", &err);

                // Audio Codec
                add_arg(args, &n, "-c:a", &err); add_arg(args, &n, "aac", &err);
                add_arg(args, &n, "-ac", &err); 
                snprintf(ac_str, sizeof(ac_str), "%d", config->audio_channels);
                add_arg(args, &n, ac_str, &err);
                
                // Output Format
                add_arg(args, &n, "-f", &err);
                if (config->backend == BACKEND_SOFTWARE && config->codec == CODEC_AV1) {
                    add_arg(args, &n, "matroska", &err);
                } else {
                    add_arg(args, &n, "mpegts", &err);
                    add_arg(args, &n, "-mpegts_flags", &err); add_arg(args, &n, "+resend_headers", &err);
                    add_arg(args, &n, "-pat_period", &err); add_arg(args, &n, "0.1", &err);
                    add_arg(args, &n, "-sdt_period", &err); add_arg(args, &n, "0.5", &err);
                }
            }
            
            add_arg(args, &n, "-", &err); // Output to stdout
            args[n] = NULL;
            
            // Check for sticky error (overflow detected during build)
            if (err) {
                LOG_ERROR("TRANSCODE", "FFmpeg argument construction failed (overflow), aborting stream");
                _exit(1);
            }
            
            execvp("ffmpeg", args);
            _exit(1);
        }
        
        // Parent of zap/ffmpeg (Stream Group Leader)
        close(zap_pipe[0]);
        close(zap_pipe[1]);
        close(pipefds[1]); // Close write end
        
        // Stream Monitor process
        close(pipefds[0]); // Monitor doesn't read
        
        // Wait for both children
        wait(NULL);
        wait(NULL);
        _exit(0);
    }
    
    // Parent
    close(zap_pipe[0]); 
    close(zap_pipe[1]); // Ensure these are closed in parent too
    
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
    
    // Use poll to monitor both pipe and socket for errors
    struct pollfd fds[2];
    fds[0].fd = pipefds[0];  // Pipe from dvbv5-zap/ffmpeg
    fds[0].events = POLLIN;
    fds[1].fd = sockfd;      // Client socket
    fds[1].events = 0;       // Only care about errors/hangup (always reported)
    
    char buffer[65536];
    ssize_t n;
    int header_sent = 0;
    
    while (1) {
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
                break;  // EOF or error
            }
            
            // Send HTTP header on first data chunk (deferred response)
            if (!header_sent) {
                if (!write_all(sockfd, http_header, strlen(http_header))) {
                    LOG_INFO("TRANSCODE", "Client disconnected before stream started");
                    break;
                }
                header_sent = 1;
            }
            
            if (!write_all(sockfd, buffer, n)) {
                LOG_INFO("TRANSCODE", "Client disconnected, killing stream group %d", pid);
                break;
            }
        }
        
        // Check for pipe errors (process died)
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            // Drain any remaining data
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
