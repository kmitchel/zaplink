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

typedef struct {
    int fd;
    char buffer[MAX_HEADER_SIZE];
    size_t total_read;
} client_context_t;

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

void send_response(int sockfd, const char *status, const char *type, const char *body) {
    char header[512];
    int len = body ? (int)strlen(body) : 0;
    snprintf(header, sizeof(header), 
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", 
        status, type, len);
    write_all(sockfd, header, strlen(header));
    if (body) write_all(sockfd, body, len);
}

// ... URL utilities ...
static void url_decode(char *s) {
    char *dst = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            int val;
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
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    char *p = strstr(query, search);
    if (!p) return 0;
    p += strlen(search);
    char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= dest_len) len = dest_len - 1;
    strncpy(dest, p, len);
    dest[len] = '\0';
    url_decode(dest);
    return 1;
}

void handle_m3u(int sockfd, const char *host, const char *query) {
    size_t cap = 64 * 1024, size = 0;
    char *m3u = malloc(cap);
    if (!m3u) {
        send_response(sockfd, "500 Internal Server Error", "text/plain", "Memory allocation failed");
        return;
    }
    strcpy(m3u, "#EXTM3U\n");
    size = strlen(m3u);

    for (int i = 0; i < channel_count; i++) {
        char entry[512];
        if (query && query[0] != '\0') {
            snprintf(entry, sizeof(entry), "#EXTINF:-1 tvg-id=\"%s\" tvg-name=\"%s\",%s %s\nhttp://%s/stream/%s?%s\n",
                     channels[i].number, channels[i].name, channels[i].number, channels[i].name, host, channels[i].number, query);
        } else {
            snprintf(entry, sizeof(entry), "#EXTINF:-1 tvg-id=\"%s\" tvg-name=\"%s\",%s %s\nhttp://%s/stream/%s\n",
                     channels[i].number, channels[i].name, channels[i].number, channels[i].name, host, channels[i].number);
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
    send_response(sockfd, "200 OK", "audio/x-mpegurl", m3u);
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
        close(sockfd);
        free(ctx);
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
        handle_m3u(sockfd, host, query);
    } else if (strcmp(path, "/xmltv.xml") == 0) {
        if (g_no_epg) {
            send_response(sockfd, "404 Not Found", "text/plain", "EPG support is disabled via command line (-n)");
        } else {
            char *xml = db_get_xmltv_programs();
            if (xml) { 
                send_response(sockfd, "200 OK", "application/xml", xml); 
                free(xml); 
            } else {
                send_response(sockfd, "500 Internal Server Error", "text/plain", "Failed to generate EPG");
            }
        }
    } else if (strncmp(path, "/stream/", 8) == 0) {
        StreamConfig config = {0};
        strncpy(config.channel_num, path + 8, sizeof(config.channel_num)-1);
        
        char b_param[64] = {0}, c_param[64] = {0}, br_param[64] = {0}, a_param[64] = {0};
        int has_b = get_query_param(query, "backend", b_param, sizeof(b_param));
        int has_c = get_query_param(query, "codec", c_param, sizeof(c_param));
        int has_br = get_query_param(query, "bitrate", br_param, sizeof(br_param));
        int has_a = get_query_param(query, "audio", a_param, sizeof(a_param));

        config.backend = has_b ? parse_backend(b_param) : BACKEND_SOFTWARE;
        config.codec = has_c ? parse_codec(c_param) : CODEC_COPY;
        config.bitrate_kbps = has_br ? atoi(br_param) : 0;

        if (has_a) {
            if (strcmp(a_param, "6") == 0 || strcmp(a_param, "5.1") == 0 || strcmp(a_param, "51") == 0) {
                config.audio_channels = 6;
            } else {
                config.audio_channels = atoi(a_param);
                if (config.audio_channels < 1 || config.audio_channels > 8) config.audio_channels = 2;
            }
        } else {
            config.audio_channels = 2;
        }

        LOG_INFO("HTTP", "Stream request: %s (backend=%s, codec=%s, bitrate=%s, audio=%dch)", 
                 config.channel_num, has_b ? b_param : "none", has_c ? c_param : "copy", 
                 config.bitrate_kbps > 0 ? br_param : "auto", config.audio_channels);

        int is_software_av1 = (config.codec == CODEC_AV1 && config.backend == BACKEND_SOFTWARE);
        char head[256];
        const char *mime = is_software_av1 ? "video/x-matroska" : "video/mp2t";
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n", mime);
        handle_unified_stream(sockfd, &config, head);
    } else {
        send_response(sockfd, "404 Not Found", "text/plain", "ZapLink Engine: Valid endpoints: /stream/{ch}, /playlist.m3u, /xmltv.xml");
    }

    close(sockfd);
    free(ctx);
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
    ev.data.fd = server_fd; // Listener
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        LOG_ERROR("HTTP", "epoll_ctl: listener");
        close(server_fd);
        close(epoll_fd);
        return;
    }

    ev.data.fd = shutdown_pipe[0];
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, shutdown_pipe[0], &ev) < 0) {
        LOG_ERROR("HTTP", "epoll_ctl: pipe");
    }

    LOG_INFO("HTTP", "Listening optimized (epoll+pool) on port %d", port);

    while (http_running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("HTTP", "epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == shutdown_pipe[0] || !http_running) {
                LOG_INFO("HTTP", "Shutdown requested");
                goto cleanup;
            }

            if (events[i].data.fd == server_fd) {
                // Accept connection
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) LOG_WARN("HTTP", "accept failed");
                    continue;
                }
                
                set_nonblocking(client_fd);
                client_context_t *ctx = calloc(1, sizeof(client_context_t));
                if (!ctx) {
                    close(client_fd);
                    continue;
                }
                ctx->fd = client_fd;

                struct epoll_event client_ev;
                client_ev.events = EPOLLIN | EPOLLET;
                client_ev.data.ptr = ctx; // Use ptr to store context
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev) < 0) {
                    close(client_fd);
                    free(ctx);
                }
            } else {
                // Read from client
                client_context_t *ctx = (client_context_t *)events[i].data.ptr;
                
                ssize_t n = read(ctx->fd, ctx->buffer + ctx->total_read, sizeof(ctx->buffer) - 1 - ctx->total_read);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    // Error or close
                    close(ctx->fd);
                    free(ctx);
                    continue;
                }
                
                ctx->total_read += n;
                ctx->buffer[ctx->total_read] = '\0';
                
                if (strstr(ctx->buffer, "\r\n\r\n")) {
                    // Full header received.
                    // Remove from epoll to stop monitoring
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->fd, NULL);
                    
                    // Dispatch to thread pool
                    if (thread_pool_submit(pool, process_client_request, ctx) < 0) {
                        LOG_WARN("HTTP", "Thread pool full, dropping request");
                        send_response(ctx->fd, "503 Service Unavailable", "text/plain", "Server busy");
                        close(ctx->fd);
                        free(ctx);
                    }
                } else if (ctx->total_read >= sizeof(ctx->buffer) - 1) {
                    send_response(ctx->fd, "431 Request Header Fields Too Large", "text/plain", "Too large");
                    close(ctx->fd);
                    free(ctx);
                }
            }
        }
    }

cleanup:
    thread_pool_destroy(pool);
    close(epoll_fd);
    close(server_fd);
    close(shutdown_pipe[0]);
    close(shutdown_pipe[1]);
    LOG_INFO("HTTP", "Server stopped");
}
