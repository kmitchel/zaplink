/**
 * @file epg.h
 * @brief Electronic Program Guide collection subsystem
 * 
 * Manages background EPG data collection from ATSC broadcasts.
 * Runs as a multi-threaded job queue that scans each mux (frequency)
 * every 15 minutes to collect VCT, EIT, and ETT table data.
 * 
 * Architecture:
 * - One orchestrator thread enqueues mux scan jobs
 * - One worker thread per tuner processes jobs concurrently
 * - Workers can be preempted by live stream requests
 */

#ifndef EPG_H
#define EPG_H

/**
 * If set to 1 before starting, skip the initial EPG scan cycle
 * Used when database already has data from a previous session
 */
extern int epg_skip_first;

/**
 * Start the EPG collection threads
 * Creates worker threads (one per tuner) and orchestrator thread
 */
void start_epg_thread();

/**
 * Stop all EPG collection threads
 * Signals workers to exit and waits for completion
 */
void stop_epg_thread();

/**
 * Block until the first complete EPG scan cycle finishes
 * Used at startup to ensure guide data is available before serving
 */
void wait_for_first_epg_scan();

#endif
