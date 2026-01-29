#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <pthread.h>
#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include <fcntl.h>
#include "http_server.h"
#include "transcode.h"
#include "channels.h"
#include "db.h"
#include "log.h"
#include "config.h"

// Connection limiting
#define MAX_CONCURRENT_CONNECTIONS 32
static sem_t connection_sem;

// Self-pipe for signal-safe shutdown
static int shutdown_pipe[2] = {-1, -1};
static volatile sig_atomic_t http_running = 1;

// Signal handler writes to pipe to wake up select()
static void http_signal_handler(int sig) {
    (void)sig;
    http_running = 0;
    // Write a byte to wake up select() - async-signal-safe
    char c = 1;
    if (shutdown_pipe[1] >= 0) {
        ssize_t n = write(shutdown_pipe[1], &c, 1);
        (void)n; // Ignore result in signal handler
    }
}

/**
 * Write all bytes to socket, handling partial writes and EINTR.
 * With SO_SNDTIMEO set, write() will timeout after the configured duration.
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

void send_response(int sockfd, const char *status, const char *type, const char *body) {
    char header[512];
    int len = body ? (int)strlen(body) : 0;
    snprintf(header, sizeof(header), 
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", 
        status, type, len);
    write_all(sockfd, header, strlen(header));
    if (body) write_all(sockfd, body, len);
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

/**
 * URL decode a string in-place (handles %XX escapes).
 */
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

void *client_thread(void *arg) {
    int sockfd = *(int*)arg;
    free(arg);
    
    char buffer[4096];
    ssize_t bytes_read = read(sockfd, buffer, sizeof(buffer)-1);
    if (bytes_read <= 0) { 
        close(sockfd); 
        sem_post(&connection_sem);
        return NULL; 
    }
    buffer[bytes_read] = '\0';
    
    // Use width specifiers to prevent buffer overflow
    char method[16], full_path[1024], protocol[16];
    if (sscanf(buffer, "%15s %1023s %15s", method, full_path, protocol) != 3) {
        send_response(sockfd, "400 Bad Request", "text/plain", "Malformed request");
        close(sockfd);
        sem_post(&connection_sem);
        return NULL;
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
        
        // Parse params
        char b_param[64] = {0}, c_param[64] = {0}, br_param[64] = {0}, a_param[64] = {0};
        int has_b = get_query_param(query, "backend", b_param, sizeof(b_param));
        int has_c = get_query_param(query, "codec", c_param, sizeof(c_param));
        int has_br = get_query_param(query, "bitrate", br_param, sizeof(br_param));
        int has_a = get_query_param(query, "audio", a_param, sizeof(a_param));

        config.backend = has_b ? parse_backend(b_param) : BACKEND_SOFTWARE;
        config.codec = has_c ? parse_codec(c_param) : CODEC_COPY;
        config.bitrate_kbps = has_br ? atoi(br_param) : 0;  // 0 = no rate control, use encoder default
        
        // Audio channels: 2 (stereo), 6 (5.1 surround). Default is 2 for transcoding.
        // Values like "5.1" or "51" are treated as 6 channels
        if (has_a) {
            if (strcmp(a_param, "6") == 0 || strcmp(a_param, "5.1") == 0 || strcmp(a_param, "51") == 0) {
                config.audio_channels = 6;
            } else {
                config.audio_channels = atoi(a_param);
                if (config.audio_channels < 1 || config.audio_channels > 8) {
                    config.audio_channels = 2;  // Fallback to stereo for invalid values
                }
            }
        } else {
            config.audio_channels = 2;  // Default to stereo
        }

        LOG_INFO("HTTP", "Stream request: %s (backend=%s, codec=%s, bitrate=%s, audio=%dch)", 
                 config.channel_num, has_b ? b_param : "none", has_c ? c_param : "copy", 
                 config.bitrate_kbps > 0 ? br_param : "auto", config.audio_channels);

        // MIME type: Only software AV1 uses Matroska, hardware AV1 uses MPEG-TS
        int is_software_av1 = (config.codec == CODEC_AV1 && config.backend == BACKEND_SOFTWARE);
        char head[256];
        const char *mime = is_software_av1 ? "video/x-matroska" : "video/mp2t";
        snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n", mime);
        // Header is passed to handle_unified_stream and sent only after stream data is ready
        handle_unified_stream(sockfd, &config, head);
    } else {
        send_response(sockfd, "404 Not Found", "text/plain", "ZapLink Engine: Valid endpoints: /stream/{ch}, /playlist.m3u, /xmltv.xml");
    }
    close(sockfd);
    sem_post(&connection_sem);
    return NULL;
}

void start_http_server(int port) {
    // Create self-pipe for shutdown signaling
    if (pipe(shutdown_pipe) < 0) {
        LOG_ERROR("HTTP", "Failed to create shutdown pipe");
        return;
    }
    // Make pipe non-blocking
    fcntl(shutdown_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(shutdown_pipe[1], F_SETFL, O_NONBLOCK);

    // Install our signal handlers
    struct sigaction sa;
    sa.sa_handler = http_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Initialize connection semaphore
    if (sem_init(&connection_sem, 0, MAX_CONCURRENT_CONNECTIONS) < 0) {
        LOG_ERROR("HTTP", "Failed to initialize connection semaphore");
        close(shutdown_pipe[0]);
        close(shutdown_pipe[1]);
        return;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR("HTTP", "Failed to create socket: %s", strerror(errno));
        sem_destroy(&connection_sem);
        close(shutdown_pipe[0]);
        close(shutdown_pipe[1]);
        return;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(port) };
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        LOG_ERROR("HTTP", "Failed to bind to port %d: %s", port, strerror(errno));
        close(server_fd);
        sem_destroy(&connection_sem);
        close(shutdown_pipe[0]);
        close(shutdown_pipe[1]);
        return;
    }
    
    if (listen(server_fd, 10) < 0) {
        LOG_ERROR("HTTP", "Failed to listen: %s", strerror(errno));
        close(server_fd);
        sem_destroy(&connection_sem);
        close(shutdown_pipe[0]);
        close(shutdown_pipe[1]);
        return;
    }

    LOG_INFO("HTTP", "Listening on port %d (max %d connections)", port, MAX_CONCURRENT_CONNECTIONS);

    while (http_running) {
        // Use select to wait on both server socket and shutdown pipe
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        FD_SET(shutdown_pipe[0], &readfds);
        int maxfd = (server_fd > shutdown_pipe[0]) ? server_fd : shutdown_pipe[0];

        int ret = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("HTTP", "select failed: %s", strerror(errno));
            break;
        }

        // Check if shutdown was requested
        if (FD_ISSET(shutdown_pipe[0], &readfds) || !http_running) {
            LOG_INFO("HTTP", "Shutdown requested, exiting accept loop");
            break;
        }

        // Check for new connection
        if (!FD_ISSET(server_fd, &readfds)) continue;

        // Wait for an available connection slot, handling EINTR
        while (sem_wait(&connection_sem) == -1) {
            if (errno != EINTR) {
                LOG_ERROR("HTTP", "sem_wait failed: %s", strerror(errno));
                goto cleanup;
            }
            if (!http_running) goto cleanup;
        }
        
        int *client_fd = malloc(sizeof(int));
        if (!client_fd) {
            LOG_WARN("HTTP", "Memory allocation failed for client");
            sem_post(&connection_sem);
            usleep(100000);
            continue;
        }
        *client_fd = accept(server_fd, NULL, NULL);
        if (*client_fd < 0) {
            free(client_fd);
            sem_post(&connection_sem);
            if (errno == EINTR && !http_running) break;
            continue;
        }
        
        // Set socket timeouts: recv and send to prevent hanging threads
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(*client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(*client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, client_fd) != 0) {
            LOG_WARN("HTTP", "Failed to create client thread");
            close(*client_fd);
            free(client_fd);
            sem_post(&connection_sem);
            continue;
        }
        pthread_detach(thread);
    }

cleanup:
    close(server_fd);
    sem_destroy(&connection_sem);
    close(shutdown_pipe[0]);
    close(shutdown_pipe[1]);
    LOG_INFO("HTTP", "Server stopped");
}
