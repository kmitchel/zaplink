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
    
    // Transcoding variables
    char v_encoder[256] = "libx264 -preset ultrafast -tune zerolatency";
    char v_hw_args[256] = "";
    char v_filter[128] = "";
    char v_rate_control[256] = "";
    int is_software_av1 = 0;
    const char *out_fmt = "mpegts";
    const char *mux_opts = "";
    int bitrate = config->bitrate_kbps;

    // Passthrough mode: minimal ffmpeg remux (copy video+audio) to clean MPEG-TS structure
    // Raw dvbv5-zap output can have incomplete PSI tables that cause slow probing
    if (config->codec == CODEC_COPY) {
        snprintf(cmd, sizeof(cmd), 
            "dvbv5-zap -c %s -a %s -o - %s | ffmpeg "
            "-fflags +genpts+discardcorrupt -analyzeduration 1M -probesize 5M "
            "-f mpegts -i - -c copy "
            "-f mpegts -mpegts_flags +resend_headers -pat_period 0.1 -sdt_period 0.5 -",
            channels_conf_path, adapter_id, c->number);
        LOG_INFO("TRANSCODE", "Starting passthrough stream: %s", config->channel_num);
    } else {
        // Transcoding mode configuration
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

        // AV1 in software needs Matroska; hardware AV1 encoders can use MPEG-TS
        out_fmt = (is_software_av1) ? "matroska" : "mpegts";
        
        // MPEG-TS options for live streaming: resend headers, frequent PSI tables
        mux_opts = (is_software_av1) ? "" : "-mpegts_flags +resend_headers -pat_period 0.1 -sdt_period 0.5";
        
        // Rate control: if bitrate > 0, use ABR; otherwise let encoder use default (typically CRF/quality)
        if (bitrate > 0) {
            if (is_software_av1) {
                // SVT-AV1 doesn't support -maxrate/-bufsize in ABR mode
                snprintf(v_rate_control, sizeof(v_rate_control), "-b:v %dk", bitrate);
            } else {
                snprintf(v_rate_control, sizeof(v_rate_control), "-b:v %dk -maxrate %dk -bufsize %dk", 
                         bitrate, bitrate * 2, bitrate * 4);
            }
        }

        snprintf(cmd, sizeof(cmd),
            "dvbv5-zap -c %s -a %s -o - %s | ffmpeg %s -fflags +genpts+discardcorrupt "
            "-analyzeduration 1000000 -probesize 1000000 -thread_queue_size 512 -i - "
            "%s -c:v %s %s -g 60 -c:a aac -ac %d -f %s %s -",
            channels_conf_path, adapter_id, c->number, v_hw_args, v_filter, v_encoder, 
            v_rate_control, config->audio_channels, out_fmt, mux_opts);

        // NOTE: c->number is used here, not config->channel_num. 
        // c->number comes from channels.conf which is trusted local data.
        // FFmpeg input flags for live streaming:
        //   -fflags +genpts+discardcorrupt: Generate PTS, discard corrupt frames
        //   -analyzeduration 1M -probesize 1M: Faster stream detection (1 second)
        //   -thread_queue_size 512: Larger input buffer for bursty input
        LOG_INFO("TRANSCODE", "Starting stream: %s", config->channel_num);
    }
    
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

            // Construct arguments array for execvp
            // We use fixed-size array since arguments are relatively static
            char *args[64];
            int i = 0;
            args[i++] = "ffmpeg";
            
            // Input options
            args[i++] = "-fflags"; args[i++] = "+genpts+discardcorrupt";
            args[i++] = "-analyzeduration"; args[i++] = "1000000";
            args[i++] = "-probesize"; args[i++] = "5000000";
            
            // Input file (stdin)
            args[i++] = "-f"; args[i++] = "mpegts";
            args[i++] = "-i"; args[i++] = "-";
            
            // Video codec
            if (config->codec == CODEC_COPY) {
                args[i++] = "-c"; args[i++] = "copy";
            } else {
                // Add hardware args if present
                if (v_hw_args[0]) {
                   // This is complex to parse back from the string - for robustness/simplicity in this refactor
                   // we might need to reconstruct the args logic or just split the string.
                   // Given the string composition above, let's keep it simple for now and rely on shell splitting?
                   // No, we promised no shell. We need to tokenize v_hw_args if we want to use them.
                   // For now, let's assume software encoding or simple flags for the approved plan scope.
                   // Correction: The user wants full robustness. I should handle the args properly.
                   // However, for this step, let's stick to the critical robust path:
                   // Since splitting complex args strings in C is error prone without a helper,
                   // and we know the exact formats from lines 112-124:
                   // "-hwaccel qsv -hwaccel_output_format qsv..."
                   // I will handle the common case (Software/Copy) robustly, and basic split for others.
                   // Actually, for this specific refactor, let's focus on the Copy path which is the stability goal.
                   // But wait, the function must support all modes.
                   // I'll use a helper or simple tokenization for the existing strings.
                   
                   // SIMPLIFICATION: To not break non-copy modes, I will tokenize the strings 
                   // constructed earlier (v_hw_args, v_filter, etc) by spaces.
                   char *token = strtok(v_hw_args, " ");
                   while (token != NULL && i < 60) {
                       args[i++] = token;
                       token = strtok(NULL, " ");
                   }
                }
                
                args[i++] = "-c:v"; args[i++] = v_encoder;
                
                if (v_filter[0]) {
                     // Filter string might contain internal quotes or spaces but usually it's -vf "filter"
                     // The logic above constructed it as: -vf "yadif..."
                     // I need to strip the -vf and just pass the filter content
                     args[i++] = "-vf";
                     // Extract just the filter part between quotes or after -vf
                     // Hacky but works for the current known strings:
                     char *f = strstr(v_filter, "vf");
                     if (f) {
                         char *start = strchr(f, '"');
                         if (start) {
                             char *end = strchr(start+1, '"');
                             if (end) *end = 0;
                             args[i++] = start+1;
                         } else {
                             // Fallback if no quotes
                             args[i++] = f + 3; 
                         }
                     }
                }
                
                if (v_rate_control[0]) {
                   char *token = strtok(v_rate_control, " ");
                   while (token && i < 60) {
                       args[i++] = token;
                       token = strtok(NULL, " ");
                   }
                }
            }
            
            // Audio codec
            args[i++] = "-c:a"; args[i++] = "aac";
            // args[i++] = "-ac"; // Need to convert int to string for audio channels
            // We can skip explicit -ac if we just rely on aac default or use the string if needed.
            // Let's create a temporary string for ac
            // char ac_str[4]; snprintf(ac_str, 4, "%d", config->audio_channels);
            // args[i++] = ac_str; -- Cannot do this safely in child after fork without pre-alloc
            // Safe bet: just omit -ac in this robust version or assume 2?
            // Wait, I can use a static/stack buffer since we are in a fresh process.
            char ac_str[8];
            sprintf(ac_str, "%d", config->audio_channels);
            args[i++] = "-ac"; args[i++] = ac_str;

            // Output format
            args[i++] = "-f"; args[i++] = (char*)out_fmt;
            
            // Mux options
            if (mux_opts && mux_opts[0]) {
               // Tokenize mux opts: "-mpegts_flags +resend_headers ..."
               // We need a non-const copy to tokenize
               char mux_copy[256];
               strncpy(mux_copy, mux_opts, 255);
               char *token = strtok(mux_copy, " ");
               while (token && i < 60) {
                   args[i++] = token;
                   token = strtok(NULL, " ");
               }
            }
            
            args[i++] = "-"; // Output to stdout
            args[i] = NULL;
            
            execvp("ffmpeg", args);
            _exit(1);
        }
        
        // Parent of zap/ffmpeg (Stream Group Leader)
        close(zap_pipe[0]);
        close(zap_pipe[1]);
        close(pipefds[1]); // Close write end
        
        // Wait for children? No, we need to exit so the REAL parent can read from pipefds[0]
        // Actually, the logic in original code was:
        // pid = fork()
        //   parent: reads pipefds[0]
        //   child: exec pipeline
        
        // In this new structure:
        // pid = fork() (Stream Monitor)
        //   parent: identical to before (reads pipefds[0])
        //   child: 
        //      forks zap
        //      forks ffmpeg
        //      exits? 
        
        // Wait, if the child exits, who reaps the zap/ffmpeg grandchildren? init?
        // If we want "pid" (the one kill sent to) to represent the group, the child must stay alive using wait().
        // BUT the parent reads from the pipe. If the child stays alive, does it interfere?
        // No, the child just needs to NOT close the write end of pipefds if it wants to keep it open?
        // Actually, ffmpeg has the write end. The Stream Monitor process (intermediate child) 
        // doesn't need to hold the pipe.
        // It can just wait() for its children.
        
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
