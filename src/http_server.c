#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include "http_server.h"
#include "transcode.h"
#include "channels.h"
#include "db.h"
#include "log.h"
#include "config.h"
#include "thread_pool.h"

#define MAX_EVENTS 64
#define MAX_HEADER_SIZE 4096
#define THREAD_POOL_SIZE 16

static int epoll_fd = -1;
static int server_fd = -1;
static int shutdown_pipe[2] = {-1, -1};
static volatile sig_atomic_t http_running = 1;
static thread_pool_t *pool = NULL;
#define MAX_STREAMS 20
static int active_streams = 0;
static int active_stream_fds[MAX_STREAMS];
static pthread_mutex_t stream_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stream_cond = PTHREAD_COND_INITIALIZER;

typedef struct client_context {
    int fd;
    char buffer[MAX_HEADER_SIZE];
    size_t total_read;
    time_t accepted_at;
    struct client_context *next;
    int managed_stream;
} client_context_t;

static client_context_t *pending_clients = NULL;
static int pending_client_count = 0;
static int listener_token;
static int shutdown_token;

void send_response(int sockfd, const char *status, const char *type,
                   const char *body);

static void send_response_ex(int sockfd, const char *status, const char *type,
                             const char *body, int head_only);

static time_t monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return now.tv_sec;
}

static void add_pending_client(client_context_t *ctx) {
    ctx->next = pending_clients;
    pending_clients = ctx;
    pending_client_count++;
}

static void remove_pending_client(client_context_t *ctx) {
    client_context_t **current = &pending_clients;
    while (*current) {
        if (*current == ctx) {
            *current = ctx->next;
            ctx->next = NULL;
            pending_client_count--;
            return;
        }
        current = &(*current)->next;
    }
}

static void close_pending_client(client_context_t *ctx) {
    if (!ctx) return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->fd, NULL);
    close(ctx->fd);
    remove_pending_client(ctx);
    free(ctx);
}

static void expire_pending_clients(void) {
    time_t now = monotonic_seconds();
    client_context_t *ctx = pending_clients;
    while (ctx) {
        client_context_t *next = ctx->next;
        if (now > 0 && ctx->accepted_at > 0 &&
            now - ctx->accepted_at >= HTTP_HEADER_TIMEOUT_SECONDS) {
            LOG_WARN("HTTP", "Closing idle request header on fd %d", ctx->fd);
            send_response(ctx->fd, "408 Request Timeout", "text/plain",
                          "Request header timeout");
            close_pending_client(ctx);
        }
        ctx = next;
    }
}

void process_client_request(void *arg);

static int reserve_stream(int fd) {
    pthread_mutex_lock(&stream_mutex);
    if (active_streams >= MAX_STREAMS) {
        pthread_mutex_unlock(&stream_mutex);
        return 0;
    }
    active_stream_fds[active_streams++] = fd;
    pthread_mutex_unlock(&stream_mutex);
    return 1;
}

static void release_stream(int fd) {
    pthread_mutex_lock(&stream_mutex);
    for (int i = 0; i < active_streams; i++) {
        if (active_stream_fds[i] != fd) continue;
        active_stream_fds[i] = active_stream_fds[active_streams - 1];
        active_streams--;
        break;
    }
    pthread_cond_broadcast(&stream_cond);
    pthread_mutex_unlock(&stream_mutex);
}

static void finish_client(client_context_t *ctx) {
    if (!ctx) return;
    if (ctx->managed_stream) release_stream(ctx->fd);
    close(ctx->fd);
    free(ctx);
}

static void stop_active_streams(void) {
    pthread_mutex_lock(&stream_mutex);
    for (int i = 0; i < active_streams; i++) {
        shutdown(active_stream_fds[i], SHUT_RDWR);
    }
    while (active_streams > 0) {
        pthread_cond_wait(&stream_cond, &stream_mutex);
    }
    pthread_mutex_unlock(&stream_mutex);
}

void *stream_worker_entry(void *arg) {
    process_client_request(arg);
    return NULL;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void http_signal_handler(int sig) {
    (void)sig;
    http_running = 0;
    char c = 1;
    if (shutdown_pipe[1] >= 0) {
        if (write(shutdown_pipe[1], &c, 1) < 0) {} 
    }
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0; 
        }
        written += n;
    }
    return 1;
}

static void send_response_ex(int sockfd, const char *status, const char *type,
                             const char *body, int head_only) {
    char header[512];
    int len = body ? (int)strlen(body) : 0;
    snprintf(header, sizeof(header), 
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", 
        status, type, len);
    write_all(sockfd, header, strlen(header));
    if (body && !head_only) write_all(sockfd, body, len);
}

void send_response(int sockfd, const char *status, const char *type, const char *body) {
    send_response_ex(sockfd, status, type, body, 0);
}

// ... URL utilities ...
static void url_decode(char *s) {
    char *dst = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            unsigned int val;
            if (sscanf(s + 1, "%2x", &val) == 1) {
                *dst++ = (char)val;
                s += 3;
                continue;
            }
        } else if (*s == '+') {
            *dst++ = ' ';
            s++;
            continue;
        }
        *dst++ = *s++;
    }
    *dst = '\0';
}

int get_query_param(const char *query, const char *key, char *dest, size_t dest_len) {
    if (!query || !key || !dest || dest_len == 0) return 0;
    size_t key_len = strlen(key);
    const char *parameter = query;
    while (*parameter) {
        const char *end = strchr(parameter, '&');
        if (!end) end = parameter + strlen(parameter);
        const char *equals = memchr(parameter, '=', (size_t)(end - parameter));
        if (equals && (size_t)(equals - parameter) == key_len &&
            strncmp(parameter, key, key_len) == 0) {
            const char *value = equals + 1;
            size_t len = (size_t)(end - value);
            if (len >= dest_len) return -1;
            memcpy(dest, value, len);
            dest[len] = '\0';
            url_decode(dest);
            return 1;
        }
        parameter = *end ? end + 1 : end;
    }
    return 0;
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

static int parse_stream_config(const char *channel_path, const char *query,
                               StreamConfig *config, char *error,
                               size_t error_size) {
    stream_config_init(config);
    TranscodeContainer path_container = OUTPUT_INVALID;
    if (channel_path &&
        !stream_config_parse_channel_path(channel_path, config->channel_num,
                                          sizeof(config->channel_num),
                                          &path_container)) {
        snprintf(error, error_size, "Invalid channel identifier");
        return 0;
    }

    char backend[64] = {0}, codec[64] = {0}, bitrate[64] = {0};
    char audio[64] = {0}, latency[64] = {0}, container[64] = {0};
    int has_backend = get_query_param(query, "backend", backend, sizeof(backend));
    int has_codec = get_query_param(query, "codec", codec, sizeof(codec));
    int has_bitrate = get_query_param(query, "bitrate", bitrate, sizeof(bitrate));
    int has_audio = get_query_param(query, "audio", audio, sizeof(audio));
    int has_latency = get_query_param(query, "latency", latency, sizeof(latency));
    int has_container = get_query_param(query, "container", container, sizeof(container));
    if (has_backend < 0 || has_codec < 0 || has_bitrate < 0 || has_audio < 0 ||
        has_latency < 0 || has_container < 0) {
        snprintf(error, error_size, "Query parameter is too long");
        return 0;
    }

    config->backend = has_backend ? parse_backend(backend) : BACKEND_SOFTWARE;
    config->codec = has_codec ? parse_codec(codec) : CODEC_COPY;
    config->latency = has_latency ? parse_latency(latency) : LATENCY_BALANCED;
    TranscodeContainer query_container = has_container
        ? parse_container(container) : OUTPUT_INVALID;
    if (config->backend == BACKEND_INVALID || config->codec == CODEC_INVALID ||
        config->latency == LATENCY_INVALID ||
        (has_container && query_container == OUTPUT_INVALID)) {
        snprintf(error, error_size, "Invalid backend, codec, container, or latency profile");
        return 0;
    }

    if (has_bitrate && !parse_bounded_int(bitrate, 1, MAX_BITRATE_KBPS,
                                           &config->bitrate_kbps)) {
        snprintf(error, error_size, "Invalid bitrate");
        return 0;
    }
    if (has_audio) {
        if (strcmp(audio, "6") == 0 || strcmp(audio, "5.1") == 0 ||
            strcmp(audio, "51") == 0) {
            config->audio_channels = 6;
        } else if (!parse_bounded_int(audio, 1, 8, &config->audio_channels)) {
            snprintf(error, error_size, "Invalid audio channel count");
            return 0;
        }
    }

    if (!stream_config_finalize(config, path_container, query_container)) {
        snprintf(error, error_size, "Container conflicts with the URL or codec");
        return 0;
    }
    return 1;
}

// M3U Cache
static char *g_m3u_cache = NULL;
static char g_m3u_cache_host[256] = {0};
static pthread_mutex_t g_m3u_mutex = PTHREAD_MUTEX_INITIALIZER;

void handle_m3u(int sockfd, const char *host, const char *query, int head_only) {
    // If query params exist, ignore cache and generate dynamic
    // Otherwise serve from cache if host matches
    
    if ((!query || query[0] == '\0') && host && host[0]) {
        pthread_mutex_lock(&g_m3u_mutex);
        if (g_m3u_cache && strcmp(g_m3u_cache_host, host) == 0) {
            send_response_ex(sockfd, "200 OK", "audio/x-mpegurl", g_m3u_cache,
                             head_only);
            pthread_mutex_unlock(&g_m3u_mutex);
            return;
        }
        pthread_mutex_unlock(&g_m3u_mutex);
    }

    StreamConfig playlist_config;
    char parse_error[160];
    if (!parse_stream_config(NULL, query, &playlist_config, parse_error,
                             sizeof(parse_error))) {
        send_response_ex(sockfd, "400 Bad Request", "text/plain", parse_error,
                         head_only);
        return;
    }

    size_t cap = 64 * 1024, size = 0;
    char *m3u = malloc(cap);
    if (!m3u) {
        send_response(sockfd, "500 Internal Server Error", "text/plain", "Memory allocation failed");
        return;
    }
    strcpy(m3u, "#EXTM3U\n");
    size = strlen(m3u);

    for (int i = 0; i < channel_count; i++) {
        char entry[2048];
        char channel_id[128];
        get_unique_channel_id(&channels[i], channel_id, sizeof(channel_id));
        if (query && query[0] != '\0') {
            snprintf(entry, sizeof(entry), "#EXTINF:-1 tvg-id=\"%.127s\" tvg-name=\"%.63s\",%.31s %.63s\nhttp://%.255s/stream/%.127s%s?%.1023s\n",
                     channel_id, channels[i].name, channels[i].number,
                     channels[i].name, host, channel_id,
                     stream_config_extension(&playlist_config), query);
        } else {
            snprintf(entry, sizeof(entry), "#EXTINF:-1 tvg-id=\"%.127s\" tvg-name=\"%.63s\",%.31s %.63s\nhttp://%.255s/stream/%.127s%s\n",
                     channel_id, channels[i].name, channels[i].number,
                     channels[i].name, host, channel_id,
                     stream_config_extension(&playlist_config));
        }
        
        size_t entry_len = strlen(entry);
        if (size + entry_len + 1 > cap) {
            cap *= 2;
            char *new_m3u = realloc(m3u, cap);
            if (!new_m3u) {
                free(m3u);
                send_response(sockfd, "500 Internal Server Error", "text/plain", "Memory allocation failed");
                return;
            }
            m3u = new_m3u;
        }
        strcpy(m3u + size, entry);
        size += entry_len;
    }
    
    send_response_ex(sockfd, "200 OK", "audio/x-mpegurl", m3u, head_only);

    // Update cache if this was a clean request (no query)
    if ((!query || query[0] == '\0') && host && host[0]) {
        pthread_mutex_lock(&g_m3u_mutex);
        if (g_m3u_cache) free(g_m3u_cache);
        g_m3u_cache = strdup(m3u);
        strncpy(g_m3u_cache_host, host, sizeof(g_m3u_cache_host)-1);
        pthread_mutex_unlock(&g_m3u_mutex);
    }

    free(m3u);
}

// Worker function: Processes fully received requests
void process_client_request(void *arg) {
    client_context_t *ctx = (client_context_t *)arg;
    int sockfd = ctx->fd;
    char *buffer = ctx->buffer;
    
    // Set back to blocking for processing logic
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);
    
    // Use timeouts for the processing phase
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Logic from old client_thread...
    
    // Parse Request
    char method[16], full_path[1024], protocol[16]; // Increased buffers just in case
    // Note: buffer is guaranteed null-terminated by the reader loop
    if (sscanf(buffer, "%15s %1023s %15s", method, full_path, protocol) != 3) {
        send_response(sockfd, "400 Bad Request", "text/plain", "Malformed request");
        finish_client(ctx);
        return;
    }
    int head_only = strcmp(method, "HEAD") == 0;
    if (strcmp(method, "GET") != 0 && !head_only) {
        send_response(sockfd, "405 Method Not Allowed", "text/plain",
                      "Only GET and HEAD are supported");
        finish_client(ctx);
        return;
    }

    // Parse Host header
    char host[256] = "localhost:18392";
    char *host_hdr = strcasestr(buffer, "Host: ");
    if (host_hdr) {
        host_hdr += 6;
        char *end = strchr(host_hdr, '\r');
        if (!end) end = strchr(host_hdr, '\n');
        if (end) {
            size_t len = end - host_hdr;
            if (len < sizeof(host)) {
                strncpy(host, host_hdr, len);
                host[len] = '\0';
            }
        }
    }

    char path[1024];
    strncpy(path, full_path, sizeof(path)-1);
    path[sizeof(path)-1] = '\0';
    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (strcmp(path, "/playlist.m3u") == 0) {
        handle_m3u(sockfd, host, query, head_only);
    } else if (strcmp(path, "/xmltv.xml") == 0) {
        if (g_no_epg) {
                send_response_ex(sockfd, "404 Not Found", "text/plain",
                                 "EPG support is disabled via command line (-n)",
                                 head_only);
        } else {
            char *xml = db_get_xmltv_programs();
            if (xml) { 
                send_response_ex(sockfd, "200 OK", "application/xml", xml,
                                 head_only);
                free(xml); 
            } else {
                send_response_ex(sockfd, "500 Internal Server Error", "text/plain",
                                 "Failed to generate EPG", head_only);
            }
        }
    } else if (strncmp(path, "/stream/", 8) == 0) {
        StreamConfig config;
        char parse_error[160];
        if (!parse_stream_config(path + 8, query, &config, parse_error,
                                 sizeof(parse_error))) {
            send_response_ex(sockfd, "400 Bad Request", "text/plain", parse_error,
                             head_only);
            finish_client(ctx);
            return;
        }
        if (!find_channel_by_id(config.channel_num)) {
            send_response_ex(sockfd, "404 Not Found", "text/plain",
                             "Channel not found or identifier is ambiguous",
                             head_only);
            finish_client(ctx);
            return;
        }

        LOG_INFO("HTTP",
                 "%s stream request: channel=%s codec=%d backend=%d container=%s latency=%s bitrate=%d audio=%dch",
                 head_only ? "HEAD" : "GET", config.channel_num, config.codec,
                 config.backend, stream_config_extension(&config),
                 stream_latency_name(config.latency), config.bitrate_kbps,
                 config.audio_channels);

        char head[384];
        snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                 "X-Zaplink-Latency: %s\r\nConnection: close\r\n"
                 "Access-Control-Allow-Origin: *\r\n\r\n",
                 stream_config_mime_type(&config),
                 stream_latency_name(config.latency));
        if (head_only) write_all(sockfd, head, strlen(head));
        else handle_unified_stream(sockfd, &config, head);
    } else {
        send_response(sockfd, "404 Not Found", "text/plain", "ZapLink Engine: Valid endpoints: /stream/{ch}, /playlist.m3u, /xmltv.xml");
    }

    finish_client(ctx);
}

void start_http_server(int port) {
    if (pipe(shutdown_pipe) < 0) {
        LOG_ERROR("HTTP", "Failed to create shutdown pipe");
        return;
    }
    set_nonblocking(shutdown_pipe[0]);
    set_nonblocking(shutdown_pipe[1]);

    struct sigaction sa;
    sa.sa_handler = http_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pool = thread_pool_create(THREAD_POOL_SIZE);
    if (!pool) {
        LOG_ERROR("HTTP", "Failed to create thread pool");
        return;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR("HTTP", "Failed to create socket: %s", strerror(errno));
        return;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(server_fd);

    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(port) };
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        LOG_ERROR("HTTP", "Failed to bind to port %d: %s", port, strerror(errno));
        close(server_fd);
        return;
    }
    
    if (listen(server_fd, 10) < 0) {
        LOG_ERROR("HTTP", "Failed to listen: %s", strerror(errno));
        close(server_fd);
        return;
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_ERROR("HTTP", "Failed to create epoll: %s", strerror(errno));
        close(server_fd);
        return;
    }

    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.ptr = &listener_token;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        LOG_ERROR("HTTP", "epoll_ctl: listener");
        close(server_fd);
        close(epoll_fd);
        return;
    }

    ev.data.ptr = &shutdown_token;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, shutdown_pipe[0], &ev) < 0) {
        LOG_ERROR("HTTP", "epoll_ctl: pipe");
    }

    LOG_INFO("HTTP", "Listening optimized (epoll+pool) on port %d", port);

    while (http_running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("HTTP", "epoll_wait failed");
            break;
        }
        if (nfds == 0) {
            expire_pending_clients();
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == &shutdown_token || !http_running) {
                LOG_INFO("HTTP", "Shutdown requested");
                goto cleanup;
            }

            if (events[i].data.ptr == &listener_token) {
                // Accept connection
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) LOG_WARN("HTTP", "accept failed");
                    continue;
                }
                if (pending_client_count >= MAX_HTTP_CONNECTIONS) {
                    send_response(client_fd, "503 Service Unavailable",
                                  "text/plain", "Too many pending connections");
                    close(client_fd);
                    continue;
                }
                
                set_nonblocking(client_fd);
                client_context_t *ctx = calloc(1, sizeof(client_context_t));
                if (!ctx) {
                    close(client_fd);
                    continue;
                }
                ctx->fd = client_fd;
                ctx->accepted_at = monotonic_seconds();

                struct epoll_event client_ev;
                client_ev.events = EPOLLIN | EPOLLET;
                client_ev.data.ptr = ctx; // Use ptr to store context
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                    close(client_fd);
                    free(ctx);
                } else {
                    add_pending_client(ctx);
                }
            } else {
                // Read from client in loop for EPOLLET
                client_context_t *ctx = (client_context_t *)events[i].data.ptr;
                
                while (1) {
                    ssize_t n = read(ctx->fd, ctx->buffer + ctx->total_read, sizeof(ctx->buffer) - 1 - ctx->total_read);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // Drained
                        }
                        // Error
                        close_pending_client(ctx);
                        break;
                    } else if (n == 0) {
                        // EOF
                        close_pending_client(ctx);
                        break;
                    }

                    ctx->total_read += n;
                    ctx->buffer[ctx->total_read] = '\0';

                if (strstr(ctx->buffer, "\r\n\r\n")) {
                    // Full header receive
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->fd, NULL);
                    remove_pending_client(ctx);
                    
                    // Pre-parse to determine dispatch target
                    char method[16], path[1024];
                    if (sscanf(ctx->buffer, "%15s %1023s", method, path) == 2) {
                        if (strncmp(path, "/stream/", 8) == 0) {
                            // Direct streaming request -> Detached thread
                            if (!reserve_stream(ctx->fd)) {
                                LOG_WARN("HTTP", "Max concurrent streams reached, dropping request");
                                send_response(ctx->fd, "503 Service Unavailable", "text/plain", "Too many streams");
                                close(ctx->fd);
                                free(ctx);
                            } else {
                                ctx->managed_stream = 1;
                                pthread_t tid;
                                if (pthread_create(&tid, NULL, stream_worker_entry, ctx) != 0) {
                                    release_stream(ctx->fd);
                                    LOG_ERROR("HTTP", "Failed to spawn stream thread");
                                    close(ctx->fd);
                                    free(ctx);
                                } else {
                                    pthread_detach(tid);
                                }
                            }
                        } else {
                            // API Request -> Thread Pool
                            if (thread_pool_submit(pool, process_client_request, ctx) < 0) {
                                LOG_WARN("HTTP", "Thread pool full, dropping request");
                                send_response(ctx->fd, "503 Service Unavailable", "text/plain", "Server busy");
                                close(ctx->fd);
                                free(ctx);
                            }
                        }
                    } else {
                         // Malformed, let the worker handle/reject it (or reject here, but worker is safer for full parsing)
                         if (thread_pool_submit(pool, process_client_request, ctx) < 0) {
                             close(ctx->fd);
                             free(ctx);
                         }
                    }
                    break; // Request dispatched
                } 
                    
                    if (ctx->total_read >= sizeof(ctx->buffer) - 1) {
                        send_response(ctx->fd, "431 Request Header Fields Too Large", "text/plain", "Too large");
                        close_pending_client(ctx);
                        break;
                    }
                }
            }
        }
        expire_pending_clients();
    }

cleanup:
    while (pending_clients) close_pending_client(pending_clients);
    stop_active_streams();
    shutdown_stream_sessions();
    thread_pool_destroy(pool);
    close(epoll_fd);
    close(server_fd);
    close(shutdown_pipe[0]);
    close(shutdown_pipe[1]);
    LOG_INFO("HTTP", "Server stopped");
}
