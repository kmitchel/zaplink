/**
 * @file channels.h
 * @brief Channel management for DVB/ATSC tuning
 * 
 * Handles loading and querying channel information from channels.conf.
 * Each channel maps a virtual channel number (e.g., "15.1") to its
 * physical frequency and service ID for tuning.
 */

#ifndef CHANNELS_H
#define CHANNELS_H

#include <sys/types.h>
#include "config.h"

/**
 * Represents a single broadcast channel
 * 
 * @field name       Station call sign (e.g., "WANE-HD")
 * @field service_id DVB service ID for this program
 * @field frequency  RF frequency in Hz (e.g., "581000000")
 * @field number     Virtual channel number (e.g., "15.1")
 */
typedef struct {
    char name[64];
    char service_id[32];
    char frequency[32];
    char number[32];
} Channel;

/** Global array of loaded channels */
extern Channel channels[MAX_CHANNELS];

/** Number of channels currently loaded */
extern int channel_count;

/** Path to channels.conf file (may be overridden at runtime) */
extern char channels_conf_path[512];

/**
 * Load channels from a DVB channels.conf file
 * @param filename Path to the channels.conf file
 * @return Number of channels loaded, or -1 on error
 */
int load_channels(const char *filename);

/**
 * Find a channel by its virtual channel number
 * @param number Virtual channel number (e.g., "15.1")
 * @return Pointer to Channel, or NULL if not found
 */
Channel *find_channel_by_number(const char *number);

/**
 * Find a channel by frequency and service ID
 * Used for accurate EPG mapping when source_id alone is ambiguous
 * @param freq     Frequency string (e.g., "581000000")
 * @param service_id DVB service ID
 * @return Pointer to Channel, or NULL if not found
 */
Channel *find_channel_by_freq_sid(const char *freq, int service_id);

/**
 * Check if a virtual channel number exists on multiple frequencies
 * This can happen when receiving stations from different markets
 * @param number Virtual channel number to check
 * @return 1 if duplicated, 0 if unique
 */
int is_vcn_duplicated(const char *number);

/**
 * Build the fast lookup map for channels
 * Should be called once after loading channels
 */
void build_channel_lookup();

/**
 * Fast lookup by frequency and service ID (O(1))
 * @param freq Frequency string
 * @param svc_id Service ID string (or number string)
 * @return Pointer to Channel or NULL
 */
Channel *find_channel_fast(const char *freq, const char *svc_id);

/**
 * Get a unique channel ID for XMLTV output (Thread-safe)
 * Writes to provided buffer.
 * @param ch Pointer to Channel
 * @param buf Output buffer
 * @param len Size of output buffer
 * @return Pointer to buf (or empty string on error)
 */
const char *get_unique_channel_id(Channel *ch, char *buf, size_t len);

#endif
