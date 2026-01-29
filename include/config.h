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
#define DEFAULT_PORT 18392
#define HTTP_PORT DEFAULT_PORT

/** Default path to DVB channel configuration file */
#define CHANNELS_CONF "channels.conf"

/** Default path to SQLite EPG database */
#define DB_PATH "epg.db"

/** Maximum number of DVB tuner adapters supported */
#define MAX_TUNERS 16

/** Maximum number of channels that can be loaded */
#define MAX_CHANNELS 200

/** Global verbose flag (defined in main.c) */
extern int g_verbose;

/** Global EPG disable flag (defined in main.c) */
extern int g_no_epg;

#endif
