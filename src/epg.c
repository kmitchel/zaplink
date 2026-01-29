/**
 * @file epg.c
 * @brief Electronic Program Guide collection from ATSC broadcasts
 * 
 * Collects EPG data by parsing ATSC PSIP (Program and System Information Protocol)
 * tables from the transport stream. Scans each frequency (mux) every 15 minutes.
 * 
 * ATSC Tables Parsed:
 * - MGT (0xC7): Master Guide Table - lists PIDs for EIT tables
 * - VCT (0xC8/0xC9): Virtual Channel Table - maps source_id to channel number
 * - EIT (0xCB): Event Information Table - program titles and times
 * - ETT (0xCC): Extended Text Table - program descriptions
 * 
 * Architecture:
 * - Orchestrator thread: Enqueues mux scan jobs, manages 15-min cycle
 * - Worker threads: One per tuner, dequeue and execute scan jobs
 * - Preemption: Workers can be interrupted by stream requests
 * 
 * Channel Mapping:
 * Uses channels.conf SERVICE_ID for accurate frequency+service_id → channel
 * mapping. This prevents cross-frequency source_id collisions where the same
 * source_id appears on different frequencies with different meanings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h> 
#include "epg.h"
#include "config.h"
#include "log.h"
#include "tuner.h"
#include "channels.h"
#include "db.h"
#include "huffman.h"

/* ============================================================================
 * Data Structures
 * ============================================================================ */

#define TS_PACKET_SIZE 188   /* MPEG-TS packet size */
#define MAX_EIT_PIDS 8       /* Max EIT PIDs to track per mux */

/**
 * Buffer for accumulating PSI/SI section data across TS packets
 * Sections can span multiple packets; this tracks reassembly state
 */
typedef struct {
    unsigned char buffer[4096];  /* Section data accumulator */
    int len;                     /* Current accumulated length */
    int expected_len;            /* Total expected section length */
    int active;                  /* Whether we're mid-section */
} SectionBuffer;

/**
 * Per-scan context - allows concurrent scanning on multiple tuners
 * Each worker thread gets its own context to avoid shared state
 */
typedef struct {
    SectionBuffer pid_buffers[8192];  /* Buffer per possible PID */
    int eit_pids[MAX_EIT_PIDS];       /* Discovered EIT PIDs from MGT */
    int eit_pid_count;                /* Number of EIT PIDs found */
    int programs_found;               /* Number of programs upserted */
    const char *freq;                 /* Current frequency being scanned */
} ScanContext;

/**
 * Source ID to channel number mapping entry
 * Built from VCT during scan, used by EIT/ETT parsers
 */
typedef struct {
    char key[64];   /* Format: "frequency_sourceid" */
    char val[16];   /* Virtual channel number (e.g., "15.1") */
} SourceMap;

/* VCT source_id → channel mapping (fallback, prefer channels.conf) */
static SourceMap source_map[256];
static int source_map_count = 0;
static pthread_mutex_t source_map_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Mux scan job - represents one frequency to scan
 */
typedef struct {
    char freq[32];     /* Frequency in Hz */
    char name[64];     /* Representative channel name */
    char number[32];   /* Representative channel number (for dvbv5-zap) */
} MuxJob;

/* Job queue for mux scans */
#define MAX_MUX_QUEUE 256
static MuxJob mux_queue[MAX_MUX_QUEUE];
static int mux_queue_head = 0;
static int mux_queue_tail = 0;
static int mux_queue_count = 0;
static int active_scan_jobs = 0;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

/* EPG thread state */
int epg_running = 0;
int epg_skip_first = 0;
static int epg_completed_cycles = 0;
static pthread_mutex_t cycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cycle_cond = PTHREAD_COND_INITIALIZER;
static pthread_t worker_threads[MAX_TUNERS];

// -----------------------------------------------------------------------------
// Prototypes
// -----------------------------------------------------------------------------

void scan_mux(Tuner *t, ScanContext *ctx, const char *channel_number, const char *channel_name);
void handle_section(ScanContext *ctx, int pid, unsigned char *section, int len);
int parse_ts_chunk(ScanContext *ctx, const unsigned char *buf, size_t len);
void parse_atsc_vct(ScanContext *ctx, unsigned char *section, int len);
void parse_atsc_eit(ScanContext *ctx, unsigned char *section, int len);
void parse_atsc_ett(ScanContext *ctx, unsigned char *section, int len);

// -----------------------------------------------------------------------------
// Source Map Helpers (Thread-Safe)
// -----------------------------------------------------------------------------

void add_source_map(const char *freq, int source_id, const char *chan_num) {
    char key[64];
    snprintf(key, sizeof(key), "%s_%d", freq, source_id);
    
    pthread_mutex_lock(&source_map_mutex);
    for(int i=0; i<source_map_count; i++) {
        if (strcmp(source_map[i].key, key) == 0) {
            pthread_mutex_unlock(&source_map_mutex);
            return;
        }
    }
    if (source_map_count < 256) {
        strcpy(source_map[source_map_count].key, key);
        strcpy(source_map[source_map_count].val, chan_num);
        source_map_count++;
    }
    pthread_mutex_unlock(&source_map_mutex);
}

int get_source_map(const char *freq, int source_id, char *result, size_t result_len) {
    if (!result || result_len == 0) return 0;
    char key[64];
    snprintf(key, sizeof(key), "%s_%d", freq, source_id);
    
    pthread_mutex_lock(&source_map_mutex);
    for(int i=0; i<source_map_count; i++) {
        if (strcmp(source_map[i].key, key) == 0) {
            strncpy(result, source_map[i].val, result_len - 1);
            result[result_len - 1] = '\0';
            pthread_mutex_unlock(&source_map_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&source_map_mutex);
    return 0;
}

int get_first_channel_on_freq(const char *freq, char *result, size_t result_len) {
    if (!result || result_len == 0) return 0;
    size_t freq_len = strlen(freq);
    pthread_mutex_lock(&source_map_mutex);
    for(int i=0; i<source_map_count; i++) {
        if (strncmp(source_map[i].key, freq, freq_len) == 0 && source_map[i].key[freq_len] == '_') {
            strncpy(result, source_map[i].val, result_len - 1);
            result[result_len - 1] = '\0';
            pthread_mutex_unlock(&source_map_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&source_map_mutex);
    return 0;
}

// -----------------------------------------------------------------------------
// Job Queue Helpers
// -----------------------------------------------------------------------------

static void enqueue_mux(const char *freq, const char *name, const char *num) {
    pthread_mutex_lock(&queue_mutex);
    if (mux_queue_count < MAX_MUX_QUEUE) {
        strcpy(mux_queue[mux_queue_tail].freq, freq);
        strcpy(mux_queue[mux_queue_tail].name, name);
        strcpy(mux_queue[mux_queue_tail].number, num);
        mux_queue_tail = (mux_queue_tail + 1) % MAX_MUX_QUEUE;
        mux_queue_count++;
        pthread_cond_signal(&queue_cond);
    }
    pthread_mutex_unlock(&queue_mutex);
}

int dequeue_mux(MuxJob *job) {
    pthread_mutex_lock(&queue_mutex);
    while (mux_queue_count == 0 && epg_running) {
        pthread_cond_wait(&queue_cond, &queue_mutex);
    }
    if (!epg_running) {
        pthread_mutex_unlock(&queue_mutex);
        return 0;
    }
    *job = mux_queue[mux_queue_head];
    mux_queue_head = (mux_queue_head + 1) % MAX_MUX_QUEUE;
    mux_queue_count--;
    active_scan_jobs++;
    pthread_mutex_unlock(&queue_mutex);
    return 1;
}

// -----------------------------------------------------------------------------
// Scanner Thread
// -----------------------------------------------------------------------------

void *scanner_worker(void *arg) {
    free(arg);
    while (epg_running) {
        MuxJob job;
        if (!dequeue_mux(&job)) break;

        Tuner *t = acquire_tuner(USER_EPG);
        if (!t) {
            // Should not happen as we have as many threads as tuners, 
            // but just in case, requeue and wait
            pthread_mutex_lock(&queue_mutex);
            active_scan_jobs--;
            pthread_mutex_unlock(&queue_mutex);
            
            usleep(1000000);
            enqueue_mux(job.freq, job.name, job.number);
            continue;
        }

        ScanContext *ctx = calloc(1, sizeof(ScanContext));
        ctx->freq = job.freq;
        
        scan_mux(t, ctx, job.number, job.name);
        
        // Decrement active jobs counter
        pthread_mutex_lock(&queue_mutex);
        active_scan_jobs--;
        pthread_mutex_unlock(&queue_mutex);

        // Re-check if we were preempted
        
        free(ctx);
        release_tuner(t);
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// EPG Thread (Orchestrator)
// -----------------------------------------------------------------------------

void *epg_orchestrator(void *arg) {
    (void)arg;
    
    // Coordination: if skipping first scan, we sleep 15 mins first.
    // If not skipping, we scan immediately.
    
    if (epg_skip_first) {
        LOG_DEBUG("EPG", "Database has data, skipping initial scan cycle");
        fflush(stdout);
        // Sleep 15 mins (broken into chunks to allow exit)
        for(int k=0; k<15*60; k++) {
            if (!epg_running) break;
            sleep(1);
        }
    }

    while (epg_running) {
        LOG_INFO("EPG", "Starting full scan cycle (fetching new program data for all channels)...");
        fflush(stdout);
        db_cleanup_expired();

        pthread_mutex_lock(&source_map_mutex);
        source_map_count = 0;
        pthread_mutex_unlock(&source_map_mutex);

        // 1. Identify unique muxes and enqueue
        char scanned_freqs[MAX_CHANNELS][32];
        int scanned_count = 0;

        for (int i = 0; i < channel_count; i++) {
            Channel *c = &channels[i];
            int already = 0;
            for(int k=0; k<scanned_count; k++) {
                if (strcmp(scanned_freqs[k], c->frequency) == 0) {
                    already = 1; break;
                }
            }
            if (already) continue;
            strcpy(scanned_freqs[scanned_count++], c->frequency);
            enqueue_mux(c->frequency, c->name, c->number);
        }

        // Wait for all jobs to be processed
        while (1) {
            pthread_mutex_lock(&queue_mutex);
            int count = mux_queue_count + active_scan_jobs;
            pthread_mutex_unlock(&queue_mutex);
            if (count == 0) break;
            sleep(1);
            if (!epg_running) break;
        }

        LOG_INFO("EPG", "Scan cycle complete");
        fflush(stdout);

        // Notify that a cycle has completed
        pthread_mutex_lock(&cycle_mutex);
        epg_completed_cycles++;
        pthread_cond_broadcast(&cycle_cond);
        pthread_mutex_unlock(&cycle_mutex);

        LOG_DEBUG("EPG", "Sleeping 15 minutes...");
        fflush(stdout);
        for(int k=0; k<15*60; k++) {
            if (!epg_running) break;
            sleep(1);
        }
    }
    return NULL;
}

void wait_for_first_epg_scan() {
    LOG_INFO("EPG", "Waiting for first scan cycle to complete...");
    fflush(stdout);
    pthread_mutex_lock(&cycle_mutex);
    while (epg_completed_cycles == 0 && epg_running) {
        pthread_cond_wait(&cycle_cond, &cycle_mutex);
    }
    pthread_mutex_unlock(&cycle_mutex);
    LOG_DEBUG("EPG", "First scan cycle detected, proceeding");
    fflush(stdout);
}

void start_epg_thread() {
    if (epg_running) return;
    epg_running = 1;
    
    // Check if we already have data to skip the initial scan
    if (db_has_data()) {
        epg_skip_first = 1;
        LOG_INFO("EPG", "Existing program data found, initial scan deferred for 15 minutes");
    } else {
        epg_skip_first = 0;
        LOG_INFO("EPG", "No program data found, starting initial scan immediately");
    }

    // Start scanner threads (one per tuner)
    for (int i = 0; i < tuner_count; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        pthread_create(&worker_threads[i], NULL, scanner_worker, id);
    }
    
    // Start orchestrator thread
    pthread_t orch_tid;
    pthread_create(&orch_tid, NULL, epg_orchestrator, NULL);
    pthread_detach(orch_tid);
}

void stop_epg_thread() {
    epg_running = 0;
    pthread_cond_broadcast(&queue_cond);
    for (int i = 0; i < tuner_count; i++) {
        pthread_join(worker_threads[i], NULL);
    }
}

// Decode ATSC Multiple String Structure (MSS)
// Ref: A/65 Section 6.10
static void atsc_mss_to_string(const unsigned char *buf, int len, char *dest, size_t dest_len) {
    if (len < 1 || !dest || dest_len == 0) return;
    dest[0] = '\0';

    int num_strings = buf[0];
    int pos = 1;

    // We take the first string for simplicity (usually there's only one, or first is preferred)
    for (int i = 0; i < num_strings; i++) {
        if (pos + 4 > len) break;
        // ISO_639_language_code (3 bytes)
        // char lang[4] = { buf[pos], buf[pos+1], buf[pos+2], 0 };
        int num_segments = buf[pos+3];
        pos += 4;

        for (int j = 0; j < num_segments; j++) {
            if (pos + 3 > len) break;
            unsigned char compr = buf[pos];
            // unsigned char mode = buf[pos+1];
            int n_bytes = buf[pos+2];
            pos += 3;

            if (pos + n_bytes > len) break;

            if (compr == 0x00) {
                // No compression
                int to_copy = (n_bytes < (int)(dest_len - strlen(dest) - 1)) ? n_bytes : (int)(dest_len - strlen(dest) - 1);
                if (to_copy > 0) {
                    size_t cur_len = strlen(dest);
                    memcpy(dest + cur_len, buf + pos, to_copy);
                    dest[cur_len + to_copy] = '\0';
                }
            } else if (compr == 0x01 || compr == 0x02) {
                // Huffman (A/65 Annex C)
                if (!huffman_decode(compr, buf + pos, n_bytes, dest, dest_len)) {
                    LOG_WARN("EPG", "Failed to decode Huffman segment type 0x%02X", compr);
                    const char *msg = "[Compressed]";
                    if (dest_len - strlen(dest) > strlen(msg) + 1) strcat(dest, msg);
                }
            }
            
            pos += n_bytes;
        }
        
        // If we processed one string, we stop (usually one lang is enough)
        break;
    }

    // Sanitize
    for (int i = 0; dest[i]; i++) {
        if ((unsigned char)dest[i] < 0x20 || (unsigned char)dest[i] > 0x7E) dest[i] = ' ';
    }
    // Trim
    int d_len = strlen(dest);
    while (d_len > 0 && dest[d_len-1] == ' ') {
        dest[--d_len] = '\0';
    }
}

// -----------------------------------------------------------------------------
// TS / PSI Parser Implementation
// -----------------------------------------------------------------------------

void handle_section(ScanContext *ctx, int pid, unsigned char *section, int len) {
    if (len < 3) return;
    unsigned char table_id = section[0];
    
    int is_eit_pid = 0;
    for (int k = 0; k < ctx->eit_pid_count; k++) {
        if (ctx->eit_pids[k] == pid) { is_eit_pid = 1; break; }
    }
    
    if (pid == 0x1FFB || is_eit_pid) {
        if (table_id == 0xC7) {
            // MGT
            int tables_defined = (section[9] << 8) | section[10];
            int loop_offset = 11;
            for(int i=0; i<tables_defined; i++) {
                if(loop_offset + 11 > len) break;
                int type = (section[loop_offset] << 8) | section[loop_offset+1];
                int t_pid = ((section[loop_offset+2] & 0x1F) << 8) | section[loop_offset+3];
                
                if (type >= 0x0100 && type <= 0x017F) {
                    if (ctx->eit_pid_count < MAX_EIT_PIDS) {
                        int found = 0;
                        for (int k = 0; k < ctx->eit_pid_count; k++) {
                            if (ctx->eit_pids[k] == t_pid) { found = 1; break; }
                        }
                        if (!found) ctx->eit_pids[ctx->eit_pid_count++] = t_pid;
                    }
                }
                int desc_len = ((section[loop_offset+9] & 0x0F) << 8) | section[loop_offset+10];
                loop_offset += 11 + desc_len;
            }
        }
        if (table_id == 0xC8 || table_id == 0xC9) {
            parse_atsc_vct(ctx, section, len);
        } else if (table_id == 0xCB) {
            parse_atsc_eit(ctx, section, len);
        } else if (table_id == 0xCC) {
            parse_atsc_ett(ctx, section, len);
        }
    }
}

int parse_ts_chunk(ScanContext *ctx, const unsigned char *buf, size_t len) {
    int packet_count = 0;
    for (size_t i = 0; i + TS_PACKET_SIZE <= len; i += TS_PACKET_SIZE) {
        if (buf[i] != 0x47) continue;

        int tei = buf[i+1] & 0x80;
        if (tei) continue;

        int pusi = buf[i+1] & 0x40;
        int pid = ((buf[i+1] & 0x1F) << 8) | buf[i+2];
        int adap = (buf[i+3] >> 4) & 0x3;
        int payload_offset = 4;

        if (adap == 0x2 || adap == 0x3) {
            int adap_len = buf[i+4];
            payload_offset += adap_len + 1;
        }

        if (payload_offset >= TS_PACKET_SIZE) continue;

        unsigned char *payload = (unsigned char*)buf + i + payload_offset;
        int payload_len = TS_PACKET_SIZE - payload_offset;

        int interesting = (pid == 0x1FFB);
        if (!interesting) {
            for (int k = 0; k < ctx->eit_pid_count; k++) {
                if (ctx->eit_pids[k] == pid) { interesting = 1; break; }
            }
        }
        if (!interesting) continue; 

        if (pusi) {
            if (payload_len < 1) continue;
            int pointer = payload[0];
            payload++; payload_len--;
            
            if (pointer < payload_len) {
                if (ctx->pid_buffers[pid].active) {
                    if (ctx->pid_buffers[pid].len + pointer < 4096) {
                        memcpy(ctx->pid_buffers[pid].buffer + ctx->pid_buffers[pid].len, payload, pointer);
                        handle_section(ctx, pid, ctx->pid_buffers[pid].buffer, ctx->pid_buffers[pid].len + pointer);
                    }
                    ctx->pid_buffers[pid].active = 0;
                }

                unsigned char *sec_start = payload + pointer;
                int sec_rem = payload_len - pointer;
                if (sec_rem >= 3) {
                    int section_len = ((sec_start[1] & 0x0F) << 8) | sec_start[2];
                    int total_len = section_len + 3;
                    
                    if (sec_rem >= total_len) {
                        handle_section(ctx, pid, sec_start, total_len);
                    } else {
                        ctx->pid_buffers[pid].len = 0;
                        memcpy(ctx->pid_buffers[pid].buffer, sec_start, sec_rem);
                        ctx->pid_buffers[pid].len = sec_rem;
                        ctx->pid_buffers[pid].expected_len = total_len;
                        ctx->pid_buffers[pid].active = 1;
                    }
                }
            }
        } else {
             if (ctx->pid_buffers[pid].active) {
                 int needed = ctx->pid_buffers[pid].expected_len - ctx->pid_buffers[pid].len;
                 int to_copy = (payload_len < needed) ? payload_len : needed;
                 memcpy(ctx->pid_buffers[pid].buffer + ctx->pid_buffers[pid].len, payload, to_copy);
                 ctx->pid_buffers[pid].len += to_copy;

                 if (ctx->pid_buffers[pid].len >= ctx->pid_buffers[pid].expected_len) {
                     handle_section(ctx, pid, ctx->pid_buffers[pid].buffer, ctx->pid_buffers[pid].len);
                     ctx->pid_buffers[pid].active = 0;
                 }
             }
        }
        packet_count++;
    }
    return packet_count;
}

// -----------------------------------------------------------------------------
// ATSC Parsing Logic
// -----------------------------------------------------------------------------

void parse_atsc_vct(ScanContext *ctx, unsigned char *section, int len) {
    int num_channels = section[9];
    int offset = 10;
    
    for (int i = 0; i < num_channels; i++) {
        if (offset + 32 > len) break;
        int major = ((section[offset + 14] & 0x0F) << 6) | ((section[offset + 15] & 0xFC) >> 2);
        int minor = ((section[offset + 15] & 0x03) << 8) | section[offset + 16];
        int source_id = (section[offset + 28] << 8) | section[offset + 29];
        
        char chan_num[16];
        snprintf(chan_num, sizeof(chan_num), "%d.%d", major, minor);
        add_source_map(ctx->freq, source_id, chan_num);

        int desc_len = ((section[offset + 30] & 0x03) << 8) | section[offset + 31];
        offset += 32 + desc_len;
    }
}

void parse_atsc_eit(ScanContext *ctx, unsigned char *section, int len) {
    int source_id = (section[3] << 8) | section[4];
    int num_events = section[9];
    int offset = 10;

    // Map source_id to channel using VCT data
    // NOTE: In ATSC, EIT source_id is NOT the same as SERVICE_ID (program_number) 
    // from channels.conf. The VCT maps source_id → virtual channel number.
    char vct_chan[16];
    if (!get_source_map(ctx->freq, source_id, vct_chan, sizeof(vct_chan))) return;
    
    Channel *ch = find_channel_by_number(vct_chan);
    if (!ch) return;
    
    const char *chan_num = ch->number;

    // Start transaction for batch processing
    db_begin_transaction();

    for (int i = 0; i < num_events; i++) {
        if (offset + 10 > len) break;
        int event_id = ((section[offset] & 0x3F) << 8) | section[offset + 1];
        unsigned int start_time_gps = (section[offset + 2] << 24) | (section[offset + 3] << 16) | (section[offset + 4] << 8) | section[offset + 5];
        int duration = ((section[offset + 6] & 0x0F) << 16) | (section[offset + 7] << 8) | section[offset + 8];
        int title_len = section[offset + 9];

        long long start_ms = ((long long)start_time_gps + 315964800LL - 18) * 1000;
        long long end_ms = start_ms + ((long long)duration * 1000);

        time_t start_sec = start_ms / 1000;
        struct tm tm_info;
        if (gmtime_r(&start_sec, &tm_info)) {
            int year = tm_info.tm_year + 1900;
            time_t now = time(NULL);
            struct tm now_tm;
            gmtime_r(&now, &now_tm);
            int current_year = now_tm.tm_year + 1900;
            if (year < 2000 || year > current_year + 2) break;
        }

        char title[256] = {0};
        if (title_len > 0) {
            int str_offset = offset + 10;
            if (str_offset + title_len <= len) {
                atsc_mss_to_string(section + str_offset, title_len, title, sizeof(title));
            }
        }

        if (title[0] != '\0' && start_ms > 0) {
            db_upsert_program(ctx->freq, chan_num, start_ms, end_ms, title, event_id, source_id);
            ctx->programs_found++;
        }

        int after_title = offset + 10 + title_len;
        if (after_title + 2 <= len) {
            int desc_len = ((section[after_title] & 0x0F) << 8) | section[after_title + 1];
            offset = after_title + 2 + desc_len;
        } else break;
    }
    
    db_commit_transaction();
}

void parse_atsc_ett(ScanContext *ctx, unsigned char *section, int len) {
    if (len < 17) return;
    int source_id_from_header = (section[3] << 8) | section[4];
    unsigned int etm_id = (section[9] << 24) | (section[10] << 16) | (section[11] << 8) | section[12];
    int event_id = (etm_id >> 2) & 0x3FFF;
    
    // Map source_id to channel using VCT data
    // NOTE: In ATSC, ETT source_id is NOT the same as SERVICE_ID (program_number)
    char chan_num[16];
    if (!get_source_map(ctx->freq, source_id_from_header, chan_num, sizeof(chan_num))) return;
    
    int section_length = ((section[1] & 0x0F) << 8) | section[2];
    int mss_start = 13;
    int mss_end = section_length + 3 - 4; // Minus CRC
    int mss_len = mss_end - mss_start;
    if (mss_len < 1 || mss_start + mss_len > len) return;
    
    char desc[1024] = {0};
    atsc_mss_to_string(section + mss_start, mss_len, desc, sizeof(desc));
    
    if (desc[0] != '\0') db_update_program_description(ctx->freq, chan_num, event_id, desc);
}

void scan_mux(Tuner *t, ScanContext *ctx, const char *channel_number, const char *channel_name) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return;
    
    // Log with both for clarity, but zap with number
    LOG_INFO("EPG", "Scanning Mux %s (%s %s) on Tuner %d", ctx->freq, channel_name, channel_number, t->id);

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        
        // Suppress stderr unless verbose mode is enabled
        if (!g_verbose) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        
        char adapter_id[8];
        snprintf(adapter_id, sizeof(adapter_id), "%d", t->id);
        execlp("dvbv5-zap", "dvbv5-zap", "-c", channels_conf_path, "-a", adapter_id, "-P", "-t", "45", "-o", "-", channel_number, NULL);
        _exit(1);
    } else if (pid > 0) {
        t->zap_pid = pid;
        close(pipefd[1]);

        unsigned char buf[1024 * 32];
        int leftover = 0;
        ssize_t n;

        while ((n = read(pipefd[0], buf + leftover, sizeof(buf) - leftover)) > 0) {
            int total_len = leftover + n;
            int packet_count = total_len / TS_PACKET_SIZE;
            int bytes_to_process = packet_count * TS_PACKET_SIZE;
            if (bytes_to_process > 0) parse_ts_chunk(ctx, buf, bytes_to_process);
            leftover = total_len - bytes_to_process;
            if (leftover > 0) memmove(buf, buf + bytes_to_process, leftover);
        }

        LOG_INFO("EPG", "Scan complete for %s %s: Found %d programs", channel_name, channel_number, ctx->programs_found);

        // If read returned < 0 and errno is not 0, we might have been preempted.
        // Actually, if release_tuner kills the process, read will return 0 or error.
        // Let's check if the child exited or was signaled.
        int status;
        waitpid(pid, &status, 0);
        t->zap_pid = 0;
        
        if (WIFSIGNALED(status)) {
            LOG_DEBUG("EPG", "Scan of %s interrupted (likely preempted)", ctx->freq);
            fflush(stdout);
            // Re-enqueue
            enqueue_mux(ctx->freq, channel_name, channel_number);
        }

        close(pipefd[0]);
    }
}
