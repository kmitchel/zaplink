#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "config.h"
#include "log.h"
#include "channels.h"
#include "db.h"
#include "tuner.h"
#include "http_server.h"
#include "epg.h"
#include "scanner.h"
#include "transcode.h"
#include "benchmark.h"

// Global configuration
int g_verbose = 0;
int g_no_epg = 0;

static int parse_port(const char *value, int *port) {
    if (!value || !port) return 0;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 1 || parsed > 65535) {
        return 0;
    }
    *port = (int)parsed;
    return 1;
}

void print_usage(const char *progname) {
    printf("ZapLink Engine - High Performance DTV Backend\n");
    printf("Usage: %s [-p port] [-v] [-t] [-s] [-n]\n", progname);
    printf("  -p port           HTTP Port (default: %d)\n", DEFAULT_PORT);
    printf("  -v                Verbose logging\n");
    printf("  -n                Disable EPG background engine\n");
    printf("  -t                Run hardware transcoding benchmark\n");
    printf("  -s                Run channel scanner setup\n");
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int opt;
    int force_scan = 0;

    while ((opt = getopt(argc, argv, "p:vhtsn")) != -1) {
        switch (opt) {
            case 'p':
                if (!parse_port(optarg, &port)) {
                    fprintf(stderr, "Invalid port: %s\n", optarg);
                    return 1;
                }
                break;
            case 'v': g_verbose = 1; break;
            case 'n': g_no_epg = 1; break;
            case 't': run_transcode_benchmark(); return 0;
            case 's': force_scan = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    // Ignore SIGPIPE (handled by checking write return values)
    signal(SIGPIPE, SIG_IGN);

    // USB DVB devices can appear a few seconds after the service starts.
    // Wait before either the scanner or the streaming engine enumerates them.
    wait_for_tuners(TUNER_WAIT_TIMEOUT_SECONDS);
    
    int scan_result = scanner_check(channels_conf_path, force_scan);
    if (scan_result > 0) return 0;
    if (scan_result < 0) return 1;

    LOG_INFO("MAIN", "ZapLink DTV engine starting on port %d", port);

    if (!g_no_epg) {
        if (!db_init()) {
            LOG_ERROR("MAIN", "Failed to initialize database");
            return 1;
        }
    }
    
    int loaded = load_channels(channels_conf_path);
    if (loaded <= 0) {
        LOG_WARN("MAIN", "No channels loaded from %s", channels_conf_path);
    } else {
        LOG_INFO("MAIN", "Loaded %d channels", loaded);
    }
    
    // Build fast lookup map for XMLTV generation
    build_channel_lookup();
    
    discover_tuners();
    if (tuner_count == 0) {
        LOG_WARN("MAIN", "No tuners found - streaming will not be available");
    }
    
    // Start EPG Background Engine
    if (!g_no_epg) {
        LOG_INFO("MAIN", "Starting EPG Engine...");
        start_epg_thread();
    }

    // Start HTTP Server (blocks until shutdown signal)
    LOG_INFO("MAIN", "Starting HTTP Interface for Jellyfin...");
    start_http_server(port);

    // Cleanup on exit (HTTP server returned due to signal)
    LOG_INFO("MAIN", "Shutting down...");
    if (!g_no_epg) {
        stop_epg_thread();
    }
    db_close();
    LOG_INFO("MAIN", "Shutdown complete");
    return 0;
}
