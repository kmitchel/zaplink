#ifndef TRANSCODE_H
#define TRANSCODE_H

#include <pthread.h>
#include "stream_config.h"

/**
 * Handle a unified stream request (Transcoded or Raw)
 * Spawns dvbv5-zap (and FFmpeg for transcoding) and pipes output to sockfd.
 * http_header is sent only after the first data chunk arrives.
 */
void handle_unified_stream(int sockfd, StreamConfig *config, const char *http_header);

/** Stop and release all reusable producer sessions. */
void shutdown_stream_sessions(void);

#endif
