#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "channels.h"
#include "config.h"
#include "log.h"
#include "stream_session.h"
#include "transcode.h"
#include "tuner.h"

typedef struct {
    pid_t process_group;
    Tuner *tuner;
    unsigned long lease_generation;
} PipelineContext;

static pthread_once_t session_init_once = PTHREAD_ONCE_INIT;
static int sessions_ready;

static int validate_channel_id(const char *value) {
    if (!value || !*value) return 0;
    for (const char *cursor = value; *cursor; cursor++) {
        if (!isdigit((unsigned char)*cursor) && *cursor != '.' && *cursor != '-') return 0;
    }
    return 1;
}

static int write_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length > 0) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        cursor += written;
        length -= (size_t)written;
    }
    return 1;
}

static void send_stream_error(int fd, const char *status, const char *message) {
    char header[512];
    size_t length = strlen(message);
    int header_length = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", status, length);
    if (header_length > 0 && (size_t)header_length < sizeof(header)) {
        write_all(fd, header, (size_t)header_length);
        write_all(fd, message, length);
    }
}

static int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void terminate_process_group(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGTERM);
    int status;
    for (int attempt = 0; attempt < 20; attempt++) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) return;
        usleep(50000);
    }
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

static void add_arg(char **arguments, int *count, const char *argument, int *error) {
    if (*error) return;
    if (*count >= 127) {
        *error = 1;
        return;
    }
    arguments[(*count)++] = (char *)argument;
    arguments[*count] = NULL;
}

static const char *find_vaapi_device(void) {
    static char device[64];
    glob_t matches;
    if (glob("/dev/dri/renderD*", GLOB_NOSORT, NULL, &matches) != 0) {
        return "/dev/dri/renderD128";
    }
    if (matches.gl_pathc > 0) snprintf(device, sizeof(device), "%s", matches.gl_pathv[0]);
    else snprintf(device, sizeof(device), "/dev/dri/renderD128");
    globfree(&matches);
    return device;
}

void build_ffmpeg_arguments(const StreamConfig *config,
                           char **arguments,
                           int *argument_error) {
    int count = 0;
    static _Thread_local char analyze[24];
    static _Thread_local char probe[24];
    static _Thread_local char audio_channels[8];
    static _Thread_local char bitrate[24];
    static _Thread_local char maxrate[24];
    static _Thread_local char bufsize[24];
    static _Thread_local char keyframe[80];

    snprintf(analyze, sizeof(analyze), "%d", config->analyze_duration_us);
    snprintf(probe, sizeof(probe), "%d", config->probe_size_bytes);
    snprintf(audio_channels, sizeof(audio_channels), "%d", config->audio_channels);
    snprintf(keyframe, sizeof(keyframe), "expr:gte(t,n_forced*%.3f)",
             config->keyframe_interval_ms / 1000.0);

    add_arg(arguments, &count, "ffmpeg", argument_error);
    add_arg(arguments, &count, "-hide_banner", argument_error);
    add_arg(arguments, &count, "-loglevel", argument_error);
    add_arg(arguments, &count, g_verbose ? "info" : "error", argument_error);
    add_arg(arguments, &count, "-fflags", argument_error);
    add_arg(arguments, &count,
            config->no_buffer ? "+genpts+discardcorrupt+nobuffer"
                              : "+genpts+discardcorrupt",
            argument_error);
    add_arg(arguments, &count, "-analyzeduration", argument_error);
    add_arg(arguments, &count, analyze, argument_error);
    add_arg(arguments, &count, "-probesize", argument_error);
    add_arg(arguments, &count, probe, argument_error);
    add_arg(arguments, &count, "-thread_queue_size", argument_error);
    add_arg(arguments, &count, "512", argument_error);
    static _Thread_local char vaapi_init[128];
    if (config->codec != CODEC_COPY) {
        switch (config->backend) {
            case BACKEND_QSV:
                add_arg(arguments, &count, "-init_hw_device", argument_error);
                add_arg(arguments, &count, "qsv=qsv:hw", argument_error);
                add_arg(arguments, &count, "-filter_hw_device", argument_error);
                add_arg(arguments, &count, "qsv", argument_error);
                add_arg(arguments, &count, "-hwaccel", argument_error);
                add_arg(arguments, &count, "qsv", argument_error);
                add_arg(arguments, &count, "-hwaccel_output_format", argument_error);
                add_arg(arguments, &count, "qsv", argument_error);
                break;
            case BACKEND_NVENC:
                add_arg(arguments, &count, "-hwaccel", argument_error);
                add_arg(arguments, &count, "cuda", argument_error);
                add_arg(arguments, &count, "-hwaccel_output_format", argument_error);
                add_arg(arguments, &count, "cuda", argument_error);
                break;
            case BACKEND_VAAPI:
                snprintf(vaapi_init, sizeof(vaapi_init), "vaapi=va:%s", find_vaapi_device());
                add_arg(arguments, &count, "-init_hw_device", argument_error);
                add_arg(arguments, &count, vaapi_init, argument_error);
                add_arg(arguments, &count, "-filter_hw_device", argument_error);
                add_arg(arguments, &count, "va", argument_error);
                add_arg(arguments, &count, "-hwaccel", argument_error);
                add_arg(arguments, &count, "vaapi", argument_error);
                add_arg(arguments, &count, "-hwaccel_output_format", argument_error);
                add_arg(arguments, &count, "vaapi", argument_error);
                add_arg(arguments, &count, "-hwaccel_device", argument_error);
                add_arg(arguments, &count, "va", argument_error);
                break;
            default:
                break;
        }
    }

    add_arg(arguments, &count, "-f", argument_error);
    add_arg(arguments, &count, "mpegts", argument_error);
    add_arg(arguments, &count, "-i", argument_error);
    add_arg(arguments, &count, "-", argument_error);

    if (config->codec == CODEC_COPY) {
        add_arg(arguments, &count, "-c", argument_error);
        add_arg(arguments, &count, "copy", argument_error);
    } else {
        add_arg(arguments, &count, "-vf", argument_error);
        switch (config->backend) {
            case BACKEND_QSV:
                add_arg(arguments, &count, "vpp_qsv=deinterlace=2", argument_error);
                break;
            case BACKEND_NVENC:
                add_arg(arguments, &count, "yadif_cuda=0:-1:1", argument_error);
                break;
            case BACKEND_VAAPI:
                add_arg(arguments, &count, "deinterlace_vaapi", argument_error);
                break;
            default:
                add_arg(arguments, &count, "yadif=0:-1:1,format=yuv420p", argument_error);
                break;
        }

        add_arg(arguments, &count, "-c:v", argument_error);
        if (config->backend == BACKEND_QSV) {
            add_arg(arguments, &count,
                    config->codec == CODEC_H264 ? "h264_qsv" :
                    config->codec == CODEC_HEVC ? "hevc_qsv" : "av1_qsv",
                    argument_error);
            if (config->codec != CODEC_AV1) {
                add_arg(arguments, &count, "-look_ahead", argument_error);
                add_arg(arguments, &count, "0", argument_error);
            }
            add_arg(arguments, &count, "-async_depth", argument_error);
            add_arg(arguments, &count, "1", argument_error);
        } else if (config->backend == BACKEND_NVENC) {
            add_arg(arguments, &count,
                    config->codec == CODEC_H264 ? "h264_nvenc" :
                    config->codec == CODEC_HEVC ? "hevc_nvenc" : "av1_nvenc",
                    argument_error);
            add_arg(arguments, &count, "-preset", argument_error);
            add_arg(arguments, &count, "p1", argument_error);
            add_arg(arguments, &count, "-tune", argument_error);
            add_arg(arguments, &count, "ll", argument_error);
            if (config->codec != CODEC_AV1) {
                add_arg(arguments, &count, "-zerolatency", argument_error);
                add_arg(arguments, &count, "1", argument_error);
            }
        } else if (config->backend == BACKEND_VAAPI) {
            add_arg(arguments, &count,
                    config->codec == CODEC_H264 ? "h264_vaapi" :
                    config->codec == CODEC_HEVC ? "hevc_vaapi" : "av1_vaapi",
                    argument_error);
            if (config->codec != CODEC_AV1) {
                add_arg(arguments, &count, "-compression_level", argument_error);
                add_arg(arguments, &count, "0", argument_error);
            }
        } else if (config->codec == CODEC_HEVC) {
            add_arg(arguments, &count, "libx265", argument_error);
            add_arg(arguments, &count, "-preset", argument_error);
            add_arg(arguments, &count, "ultrafast", argument_error);
        } else if (config->codec == CODEC_AV1) {
            add_arg(arguments, &count, "libsvtav1", argument_error);
            add_arg(arguments, &count, "-preset", argument_error);
            add_arg(arguments, &count, "12", argument_error);
        } else {
            add_arg(arguments, &count, "libx264", argument_error);
            add_arg(arguments, &count, "-preset", argument_error);
            add_arg(arguments, &count, "ultrafast", argument_error);
            add_arg(arguments, &count, "-tune", argument_error);
            add_arg(arguments, &count, "zerolatency", argument_error);
        }

        if (config->bitrate_kbps > 0) {
            snprintf(bitrate, sizeof(bitrate), "%dk", config->bitrate_kbps);
            snprintf(maxrate, sizeof(maxrate), "%dk", config->bitrate_kbps * 2);
            snprintf(bufsize, sizeof(bufsize), "%dk", config->bitrate_kbps * 4);
            add_arg(arguments, &count, "-b:v", argument_error);
            add_arg(arguments, &count, bitrate, argument_error);
            if (config->backend != BACKEND_SOFTWARE || config->codec != CODEC_AV1) {
                add_arg(arguments, &count, "-maxrate", argument_error);
                add_arg(arguments, &count, maxrate, argument_error);
                add_arg(arguments, &count, "-bufsize", argument_error);
                add_arg(arguments, &count, bufsize, argument_error);
            }
        }

        add_arg(arguments, &count, "-force_key_frames", argument_error);
        add_arg(arguments, &count, keyframe, argument_error);
        add_arg(arguments, &count, "-c:a", argument_error);
        add_arg(arguments, &count, "aac", argument_error);
        add_arg(arguments, &count, "-ac", argument_error);
        add_arg(arguments, &count, audio_channels, argument_error);
    }

    add_arg(arguments, &count, "-f", argument_error);
    if (config->container == OUTPUT_MATROSKA) {
        add_arg(arguments, &count, "matroska", argument_error);
    } else {
        add_arg(arguments, &count, "mpegts", argument_error);
        add_arg(arguments, &count, "-mpegts_flags", argument_error);
        add_arg(arguments, &count, "+resend_headers+initial_discontinuity", argument_error);
        add_arg(arguments, &count, "-pat_period", argument_error);
        add_arg(arguments, &count, "0.1", argument_error);
        add_arg(arguments, &count, "-sdt_period", argument_error);
        add_arg(arguments, &count, "0.5", argument_error);
        add_arg(arguments, &count, "-flush_packets", argument_error);
        add_arg(arguments, &count, "1", argument_error);
    }
    add_arg(arguments, &count, "-", argument_error);
}

static int start_pipeline(const StreamConfig *config, StreamProducer *producer) {
    if (!validate_channel_id(config->channel_num)) return -1;
    Channel *channel = find_channel_by_id(config->channel_num);
    if (!channel) return -1;

    unsigned long lease_generation = 0;
    Tuner *tuner = acquire_tuner(USER_STREAM, &lease_generation);
    if (!tuner) return -1;

    int zap_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe(zap_pipe) < 0 || pipe(output_pipe) < 0) {
        if (zap_pipe[0] >= 0) close(zap_pipe[0]);
        if (zap_pipe[1] >= 0) close(zap_pipe[1]);
        if (output_pipe[0] >= 0) close(output_pipe[0]);
        if (output_pipe[1] >= 0) close(output_pipe[1]);
        release_tuner(tuner, lease_generation);
        return -1;
    }

    pid_t group = fork();
    if (group < 0) {
        close(zap_pipe[0]); close(zap_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        release_tuner(tuner, lease_generation);
        return -1;
    }

    if (group == 0) {
        if (setpgid(0, 0) < 0) _exit(1);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        /* Pipeline shutdown closes pipes by design. Keep the resulting
         * dvbv5-zap/FFmpeg diagnostics out of normal service logs, matching
         * the historical non-verbose behavior. */
        if (!g_verbose) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        pid_t zap = fork();
        if (zap == 0) {
            close(output_pipe[0]); close(output_pipe[1]); close(zap_pipe[0]);
            dup2(zap_pipe[1], STDOUT_FILENO);
            close(zap_pipe[1]);
            char adapter[16];
            snprintf(adapter, sizeof(adapter), "%d", tuner->id);
            execlp("dvbv5-zap", "dvbv5-zap", "-c", channels_conf_path,
                   "-a", adapter, "-p", "-o", "-", channel->number, NULL);
            _exit(1);
        }
        if (zap < 0) _exit(1);

        pid_t ffmpeg = fork();
        if (ffmpeg == 0) {
            close(output_pipe[0]); close(zap_pipe[1]);
            dup2(zap_pipe[0], STDIN_FILENO);
            dup2(output_pipe[1], STDOUT_FILENO);
            close(zap_pipe[0]); close(output_pipe[1]);
            char *arguments[128] = {0};
            int argument_error = 0;
            build_ffmpeg_arguments(config, arguments, &argument_error);
            if (argument_error) _exit(1);
            execvp("ffmpeg", arguments);
            _exit(1);
        }
        close(zap_pipe[0]); close(zap_pipe[1]);
        close(output_pipe[0]); close(output_pipe[1]);
        if (ffmpeg < 0) {
            kill(zap, SIGTERM);
            waitpid(zap, NULL, 0);
            _exit(1);
        }
        waitpid(zap, NULL, 0);
        waitpid(ffmpeg, NULL, 0);
        _exit(0);
    }

    if (setpgid(group, group) < 0 && errno != EACCES && errno != ESRCH) {
        LOG_WARN("TRANSCODE", "Unable to set process group %d: %s", group, strerror(errno));
    }
    close(zap_pipe[0]); close(zap_pipe[1]); close(output_pipe[1]);

    PipelineContext *context = calloc(1, sizeof(*context));
    if (!context) {
        close(output_pipe[0]);
        terminate_process_group(group);
        release_tuner(tuner, lease_generation);
        return -1;
    }
    context->process_group = group;
    context->tuner = tuner;
    context->lease_generation = lease_generation;
    producer->fd = output_pipe[0];
    producer->opaque = context;
    LOG_INFO("TRANSCODE",
             "Producer started: channel=%s codec=%d backend=%d container=%s adapter=%d",
             config->channel_num, config->codec, config->backend,
             stream_config_extension(config), tuner->id);
    return 0;
}

static void stop_pipeline(StreamProducer *producer) {
    if (!producer) return;
    if (producer->fd >= 0) {
        close(producer->fd);
        producer->fd = -1;
    }
    PipelineContext *context = producer->opaque;
    if (context) {
        terminate_process_group(context->process_group);
        release_tuner(context->tuner, context->lease_generation);
        LOG_INFO("TRANSCODE", "Producer stopped (group=%d)", context->process_group);
        free(context);
    }
    producer->opaque = NULL;
}

static void initialize_sessions(void) {
    const StreamProducerOps operations = {
        .start = start_pipeline,
        .stop = stop_pipeline
    };
    sessions_ready = stream_sessions_init(&operations);
}

static void configure_stream_socket(int socket_fd) {
    int enabled = 1;
    int keepidle = 10;
    int keepintvl = 5;
    int keepcnt = 3;
    int send_buffer = 256 * 1024;
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
    setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

void handle_unified_stream(int socket_fd,
                           StreamConfig *config,
                           const char *http_header) {
    pthread_once(&session_init_once, initialize_sessions);
    if (!sessions_ready) {
        send_stream_error(socket_fd, "500 Internal Server Error",
                          "Stream session manager is unavailable");
        return;
    }

    uint64_t cursor = 0;
    StreamSession *session = stream_session_acquire(config, &cursor);
    if (!session) {
        send_stream_error(socket_fd, "503 Service Unavailable",
                          "No stream session is available");
        return;
    }

    configure_stream_socket(socket_fd);
    unsigned char buffer[65536];
    int header_sent = 0;
    int64_t started = monotonic_milliseconds();
    int64_t last_data = started;

    while (1) {
        ssize_t bytes = stream_session_read(session, &cursor, buffer,
                                            sizeof(buffer), 1000);
        if (bytes > 0) {
            if (!header_sent) {
                if (!write_all(socket_fd, http_header, strlen(http_header))) break;
                header_sent = 1;
            }
            if (!write_all(socket_fd, buffer, (size_t)bytes)) break;
            last_data = monotonic_milliseconds();
            continue;
        }
        if (bytes == -2) {
            LOG_WARN("SESSION", "Slow subscriber overran buffer for %s",
                     config->channel_num);
            break;
        }
        if (bytes < 0) {
            if (!header_sent) {
                send_stream_error(socket_fd, "502 Bad Gateway",
                                  stream_session_error(session));
            }
            break;
        }

        struct pollfd client = { .fd = socket_fd, .events = 0 };
        if (poll(&client, 1, 0) > 0 &&
            client.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }
        int64_t now = monotonic_milliseconds();
        int timeout = header_sent ? STREAM_STALL_TIMEOUT_SECONDS
                                  : STREAM_START_TIMEOUT_SECONDS;
        int64_t reference = header_sent ? last_data : started;
        if (now > 0 && reference > 0 && now - reference >= timeout * 1000LL) {
            if (!header_sent) {
                send_stream_error(socket_fd, "504 Gateway Timeout",
                                  "Timed out waiting for broadcast data");
            }
            break;
        }
    }

    stream_session_release(session);
    LOG_INFO("SESSION", "Subscriber ended for %s", config->channel_num);
}

void shutdown_stream_sessions(void) {
    if (sessions_ready) stream_sessions_shutdown();
}
