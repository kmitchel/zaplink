/**
 * @file config.h
 * @brief Global configuration constants for ZapLink
 * 
 * Defines compile-time configuration values including network ports,
 * file paths, and resource limits. These can be overridden at compile
 * time with -D flags if needed.
 */

#ifndef CONFIG_H
#define CONFIG_H

/** Default HTTP server port */
#ifndef DEFAULT_PORT
#define DEFAULT_PORT 18392
#endif
#define HTTP_PORT DEFAULT_PORT

/** Default path to DVB channel configuration file */
#ifndef CHANNELS_CONF
#define CHANNELS_CONF "channels.conf"
#endif

/** Default path to SQLite EPG database */
#ifndef DB_PATH
#define DB_PATH "epg.db"
#endif

/** Maximum number of DVB tuner adapters supported */
#ifndef MAX_TUNERS
#define MAX_TUNERS 16
#endif

/** Maximum number of channels that can be loaded */
#ifndef MAX_CHANNELS
#define MAX_CHANNELS 200
#endif

/** Maximum time to wait for a usable DVB frontend during startup */
#ifndef TUNER_WAIT_TIMEOUT_SECONDS
#define TUNER_WAIT_TIMEOUT_SECONDS 30
#endif

/** Minimum ATSC carrier-to-noise ratio retained by the channel scanner */
#ifndef MIN_RELIABLE_CNR_DB
#define MIN_RELIABLE_CNR_DB 20.0
#endif

/** Maximum time to wait for initial stream data and later stalled data. */
#ifndef STREAM_START_TIMEOUT_SECONDS
#define STREAM_START_TIMEOUT_SECONDS 15
#endif
#ifndef STREAM_STALL_TIMEOUT_SECONDS
#define STREAM_STALL_TIMEOUT_SECONDS 15
#endif

/** HTTP resource and input limits. */
#ifndef HTTP_HEADER_TIMEOUT_SECONDS
#define HTTP_HEADER_TIMEOUT_SECONDS 10
#endif
#ifndef MAX_HTTP_CONNECTIONS
#define MAX_HTTP_CONNECTIONS 256
#endif
#ifndef MAX_BITRATE_KBPS
#define MAX_BITRATE_KBPS 50000
#endif

/** Global verbose flag (defined in main.c) */
extern int g_verbose;

/** Global EPG disable flag (defined in main.c) */
extern int g_no_epg;

#endif
