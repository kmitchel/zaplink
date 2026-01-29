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
 * Get a unique channel ID for XMLTV output
 * Returns simple format ("16.1") normally, or with MHz suffix ("16.1-527")
 * if the same VCN exists on multiple frequencies
 * @param ch Pointer to Channel
 * @return Static string with unique channel ID
 */
const char *get_unique_channel_id(Channel *ch);

#endif
