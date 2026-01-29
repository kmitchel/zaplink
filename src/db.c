/**
 * @file db.c
 * @brief SQLite database implementation for EPG storage
 * 
 * Stores and retrieves Electronic Program Guide data. The programs
 * table uses a composite primary key (frequency, channel, start_time)
 * to uniquely identify each program entry.
 * 
 * Output formats:
 * - XMLTV: Standard format for EPG interchange, compatible with Jellyfin/Plex
 * - JSON: Lightweight format for web clients
 * 
 * The database is stored in the working directory as epg.db.
 * Expired entries (ended > 24 hours ago) are periodically cleaned up.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <time.h>
#include <pthread.h>
#include "db.h"
#include "config.h"
#include "channels.h"
#include "log.h"

/* SQLite database connection handle */
sqlite3 *db = NULL;

/* Prepared statements for batched operations */
static sqlite3_stmt *stmt_upsert = NULL;
static sqlite3_stmt *stmt_update_desc = NULL;
static pthread_mutex_t db_stmt_mutex = PTHREAD_MUTEX_INITIALIZER;

int db_init() {
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    
    // Check constraints are off by default in some older sqlites, ensure foreign keys are on if needed (not needed yet)
    // Synchronous = NORMAL is safer than OFF but faster than FULL. 
    // WAL mode usually better but requires /dev/shm which might be restricted in some containers.
    // Stick to default pending performance profile.
    
    // Create Table if not exists
    char *sql = "CREATE TABLE IF NOT EXISTS programs ("
                "frequency TEXT, "
                "channel_service_id TEXT, "
                "start_time INTEGER, "
                "end_time INTEGER, "
                "title TEXT, "
                "description TEXT, "
                "event_id INTEGER, "
                "source_id INTEGER, "
                "PRIMARY KEY (frequency, channel_service_id, start_time));";
    
    char *err_msg = 0;
    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }

    // Index for title to speed up series detection (counting occurrences)
    char *sql_idx = "CREATE INDEX IF NOT EXISTS idx_programs_title ON programs(title);";
    rc = sqlite3_exec(db, sql_idx, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error creating index: %s\n", err_msg);
        sqlite3_free(err_msg);
    }
    
    return 1;
}

void db_close() {
    pthread_mutex_lock(&db_stmt_mutex);
    if (stmt_upsert) { sqlite3_finalize(stmt_upsert); stmt_upsert = NULL; }
    if (stmt_update_desc) { sqlite3_finalize(stmt_update_desc); stmt_update_desc = NULL; }
    pthread_mutex_unlock(&db_stmt_mutex);

    if (db) sqlite3_close(db);
}

// Transaction control
void db_begin_transaction() {
    if (!db) return;
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, &err) != SQLITE_OK) {
        LOG_WARN("DB", "Failed to begin transaction: %s", err);
        sqlite3_free(err);
    }
}

void db_commit_transaction() {
    if (!db) return;
    char *err = NULL;
    if (sqlite3_exec(db, "COMMIT;", 0, 0, &err) != SQLITE_OK) {
        LOG_WARN("DB", "Failed to commit transaction: %s", err);
        sqlite3_free(err);
    }
}

int db_has_data() {
    if (!db) return 0;
    const char *sql = "SELECT COUNT(*) FROM programs;";
    sqlite3_stmt *stmt;
    int has_data = 0;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            if (count > 0) has_data = 1;
        }
        sqlite3_finalize(stmt);
    }
    return has_data;
}

// Helper to append to dynamic string. Returns 1 on success, 0 on OOM.
int append_str(char **dest, size_t *size, size_t *cap, const char *src) {
    size_t len = strlen(src);
    if (*size + len + 1 > *cap) {
        size_t new_cap = (*size + len + 1) * 2;
        // Sometimes a large jump is better if we are growing huge
        if (new_cap < *cap + 1024*1024) new_cap = *cap + 1024*1024; 
        
        char *new_dest = realloc(*dest, new_cap);
        if (!new_dest) return 0; // OOM
        *dest = new_dest;
        *cap = new_cap;
    }
    strcpy(*dest + *size, src);
    *size += len;
    return 1;
}

// Returns 0 on OOM, 1 on success
static int xml_escape_append(char **dest, size_t *size, size_t *cap, const char *src) {
    if (!src) return 1;
    for (const char *p = src; *p; p++) {
        int ok;
        switch (*p) {
            case '&':  ok = append_str(dest, size, cap, "&amp;"); break;
            case '<':  ok = append_str(dest, size, cap, "&lt;"); break;
            case '>':  ok = append_str(dest, size, cap, "&gt;"); break;
            case '"':  ok = append_str(dest, size, cap, "&quot;"); break;
            case '\'': ok = append_str(dest, size, cap, "&apos;"); break;
            default:   ok = append_str(dest, size, cap, (char[]){*p, 0}); break;
        }
        if (!ok) return 0;
    }
    return 1;
}

// Macro to bail on OOM
#define APPEND_OR_FAIL(expr) do { if (!(expr)) goto oom_fail; } while(0)

char *db_get_xmltv_programs() {
    if (!db) return NULL;

    // Optimization: Get count first to pre-size buffer
    int row_count = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    
    // Pre-count query
    sqlite3_stmt *count_stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM programs WHERE end_time > ?", -1, &count_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(count_stmt, 1, now_ms);
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            row_count = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
    }
    
    // Estimate size: ~600 bytes per program entry + header/footer
    // If 0 rows, use default small cap
    size_t cap = (row_count > 0) ? (row_count * 600 + 4096) : (64 * 1024);
    size_t size = 0;
    char *xml = malloc(cap);
    if (!xml) return NULL;
    xml[0] = '\0';

    APPEND_OR_FAIL(append_str(&xml, &size, &cap, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n<tv generator-info-name=\"ZapLink\">\n"));

    for (int i = 0; i < channel_count; i++) {
        const char *unique_id = get_unique_channel_id(&channels[i]);
        char buf[256];
        snprintf(buf, sizeof(buf), "  <channel id=\"%s\">\n    <display-name>", unique_id);
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, buf));
        APPEND_OR_FAIL(xml_escape_append(&xml, &size, &cap, channels[i].name));
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "</display-name>\n  </channel>\n"));
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT title, description, start_time, end_time, channel_service_id, frequency, event_id, "
                      "(SELECT COUNT(*) FROM programs p2 WHERE p2.title = programs.title) as title_count "
                      "FROM programs "
                      "WHERE end_time > ? "
                      "ORDER BY CAST(SUBSTR(channel_service_id, 1, INSTR(channel_service_id, '.') - 1) AS INTEGER), "
                      "CAST(SUBSTR(channel_service_id, INSTR(channel_service_id, '.') + 1) AS INTEGER), start_time;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        free(xml);
        return NULL;
    }

    sqlite3_bind_int64(stmt, 1, now_ms);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *title = (const char *)sqlite3_column_text(stmt, 0);
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        long long start = sqlite3_column_int64(stmt, 2);
        long long end = sqlite3_column_int64(stmt, 3);
        const char *svc_id = (const char *)sqlite3_column_text(stmt, 4);
        const char *freq = (const char *)sqlite3_column_text(stmt, 5);
        int event_id = sqlite3_column_int(stmt, 6);

        Channel *ch = NULL;
        if (freq && svc_id) {
            for (int i = 0; i < channel_count; i++) {
                if (strcmp(channels[i].frequency, freq) == 0 && 
                    strcmp(channels[i].number, svc_id) == 0) {
                    ch = &channels[i];
                    break;
                }
            }
        }
        const char *channel_id = ch ? get_unique_channel_id(ch) : (svc_id ? svc_id : "");

        time_t start_s = start / 1000;
        time_t end_s = end / 1000;
        struct tm tm_s_buf, tm_e_buf;
        gmtime_r(&start_s, &tm_s_buf);
        char start_str[32];
        strftime(start_str, 32, "%Y%m%d%H%M%S +0000", &tm_s_buf);
        
        gmtime_r(&end_s, &tm_e_buf);
        char end_str[32];
        strftime(end_str, 32, "%Y%m%d%H%M%S +0000", &tm_e_buf);

        char buf[512];
        snprintf(buf, sizeof(buf), "  <programme start=\"%s\" stop=\"%s\" channel=\"%s\">\n", 
                 start_str, end_str, channel_id);
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, buf));
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "    <title>"));
        APPEND_OR_FAIL(xml_escape_append(&xml, &size, &cap, title));
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "</title>\n"));
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "    <desc>"));
        APPEND_OR_FAIL(xml_escape_append(&xml, &size, &cap, desc));
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "</desc>\n"));
        
        char ep_str[64];
        snprintf(ep_str, sizeof(ep_str), "    <episode-num system=\"xmltv_ns\">0.%d.</episode-num>\n", event_id);
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, ep_str));
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "    <new />\n"));
        
        int title_count = sqlite3_column_int(stmt, 7);
        if (title_count > 1) {
            APPEND_OR_FAIL(append_str(&xml, &size, &cap, "    <category>Series</category>\n"));
        }
        
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "  </programme>\n"));
    }

    APPEND_OR_FAIL(append_str(&xml, &size, &cap, "</tv>"));
    
    sqlite3_finalize(stmt);
    return xml;

oom_fail:
    free(xml);
    sqlite3_finalize(stmt);
    LOG_ERROR("DB", "OOM while generating XMLTV");
    return NULL;
}

// Returns 0 on OOM, 1 on success
static int json_escape_append(char **dest, size_t *size, size_t *cap, const char *src) {
    if (!src) return 1;
    for (const char *p = src; *p; p++) {
        int ok;
        char buf[8];
        switch (*p) {
            case '"':  ok = append_str(dest, size, cap, "\\\""); break;
            case '\\': ok = append_str(dest, size, cap, "\\\\"); break;
            case '\n': ok = append_str(dest, size, cap, "\\n"); break;
            case '\r': ok = append_str(dest, size, cap, "\\r"); break;
            case '\t': ok = append_str(dest, size, cap, "\\t"); break;
            default:
                 if ((unsigned char)*p < 0x20) {
                     snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
                     ok = append_str(dest, size, cap, buf);
                 } else {
                     buf[0] = *p; buf[1] = '\0';
                     ok = append_str(dest, size, cap, buf);
                 }
                 break;
        }
        if (!ok) return 0;
    }
    return 1;
}

char *db_get_json_programs() {
    if (!db) return NULL;

    const char *sql = "SELECT title, description, start_time, end_time, channel_service_id FROM programs "
                "WHERE end_time > ? "
                "ORDER BY CAST(channel_service_id AS INTEGER), "
                "CASE WHEN INSTR(channel_service_id, '.') > 0 THEN CAST(SUBSTR(channel_service_id, INSTR(channel_service_id, '.') + 1) AS INTEGER) ELSE 0 END, "
                "start_time";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    
    if (rc != SQLITE_OK) {
        LOG_ERROR("DB", "Failed to fetch data: %s", sqlite3_errmsg(db));
        return NULL;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    sqlite3_bind_int64(stmt, 1, now_ms);

    size_t cap = 1024 * 1024;
    size_t size = 0;
    char *json = malloc(cap);
    if (!json) { sqlite3_finalize(stmt); return NULL; }
    json[0] = '\0';

    APPEND_OR_FAIL(append_str(&json, &size, &cap, "{\n  \"channels\": [\n"));

    for (int i = 0; i < channel_count; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "    {\"id\": \"%s\", \"name\": \"", channels[i].number);
        APPEND_OR_FAIL(append_str(&json, &size, &cap, buf));
        APPEND_OR_FAIL(json_escape_append(&json, &size, &cap, channels[i].name));
        APPEND_OR_FAIL(append_str(&json, &size, &cap, "\"}"));
        if (i < channel_count - 1) APPEND_OR_FAIL(append_str(&json, &size, &cap, ","));
        APPEND_OR_FAIL(append_str(&json, &size, &cap, "\n"));
    }

    APPEND_OR_FAIL(append_str(&json, &size, &cap, "  ],\n  \"programs\": [\n"));

    int first = 1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *title = (const char *)sqlite3_column_text(stmt, 0);
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        long long start = sqlite3_column_int64(stmt, 2);
        long long end = sqlite3_column_int64(stmt, 3);
        const char *svc_id = (const char *)sqlite3_column_text(stmt, 4);

        if (!first) APPEND_OR_FAIL(append_str(&json, &size, &cap, ",\n"));
        first = 0;

        char buf[256];
        snprintf(buf, sizeof(buf), "    {\"channel\": \"%s\", \"start\": %lld, \"end\": %lld, \"title\": \"",
            svc_id ? svc_id : "", start, end);
        APPEND_OR_FAIL(append_str(&json, &size, &cap, buf));
        APPEND_OR_FAIL(json_escape_append(&json, &size, &cap, title));
        APPEND_OR_FAIL(append_str(&json, &size, &cap, "\", \"description\": \""));
        APPEND_OR_FAIL(json_escape_append(&json, &size, &cap, desc));
        APPEND_OR_FAIL(append_str(&json, &size, &cap, "\"}"));
    }

    APPEND_OR_FAIL(append_str(&json, &size, &cap, "\n  ]\n}"));
    
    sqlite3_finalize(stmt);
    return json;

oom_fail:
    free(json);
    sqlite3_finalize(stmt);
    LOG_ERROR("DB", "OOM while generating JSON");
    return NULL;
}


void db_upsert_program(const char *frequency, const char *channel_service_id, long long start_time, long long end_time, const char *title, int event_id, int source_id) {
    if (!db) return;

    pthread_mutex_lock(&db_stmt_mutex);
    
    if (!stmt_upsert) {
        char *sql = "INSERT INTO programs (frequency, channel_service_id, start_time, end_time, title, description, event_id, source_id) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                    "ON CONFLICT(frequency, channel_service_id, start_time) "
                    "DO UPDATE SET title=excluded.title, end_time=excluded.end_time, event_id=excluded.event_id, source_id=excluded.source_id";
        
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_upsert, 0) != SQLITE_OK) {
            LOG_ERROR("DB", "Failed to prepare upsert stmt: %s", sqlite3_errmsg(db));
            pthread_mutex_unlock(&db_stmt_mutex);
            return;
        }
    }

    // Reset before bind (in case previous exec failed or normal reuse)
    // Actually sqlite3_reset should be called after step, but safe to call here if needed?
    // Standard pattern is: bind, step, reset.
    // If we crash mid-step, the statement might remain busy.
    // But we are in a mutex.
    
    sqlite3_bind_text(stmt_upsert, 1, frequency, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_upsert, 2, channel_service_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_upsert, 3, start_time);
    sqlite3_bind_int64(stmt_upsert, 4, end_time);
    sqlite3_bind_text(stmt_upsert, 5, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_upsert, 6, "", -1, SQLITE_STATIC); // Description empty for now
    sqlite3_bind_int(stmt_upsert, 7, event_id);
    sqlite3_bind_int(stmt_upsert, 8, source_id);

    if (sqlite3_step(stmt_upsert) != SQLITE_DONE) {
         LOG_ERROR("DB", "Upsert step failed: %s", sqlite3_errmsg(db));
    }
    
    sqlite3_reset(stmt_upsert);
    sqlite3_clear_bindings(stmt_upsert);
    
    pthread_mutex_unlock(&db_stmt_mutex);
}

void db_update_program_description(const char *frequency, const char *channel_service_id, int event_id, const char *description) {
    if (!db || !description || description[0] == '\0') return;

    pthread_mutex_lock(&db_stmt_mutex);

    if (!stmt_update_desc) {
         char *sql = "UPDATE programs SET description = ? WHERE frequency = ? AND channel_service_id = ? AND event_id = ?";
         if (sqlite3_prepare_v2(db, sql, -1, &stmt_update_desc, 0) != SQLITE_OK) {
             LOG_ERROR("DB", "Failed to prepare update_desc stmt: %s", sqlite3_errmsg(db));
             pthread_mutex_unlock(&db_stmt_mutex);
             return;
         }
    }

    sqlite3_bind_text(stmt_update_desc, 1, description, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_update_desc, 2, frequency, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_update_desc, 3, channel_service_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_update_desc, 4, event_id);

    if (sqlite3_step(stmt_update_desc) != SQLITE_DONE) {
        LOG_ERROR("DB", "Update desc step failed: %s", sqlite3_errmsg(db));
    }

    sqlite3_reset(stmt_update_desc);
    sqlite3_clear_bindings(stmt_update_desc);

    pthread_mutex_unlock(&db_stmt_mutex);
}

// Delete program entries that ended more than 24 hours ago
int db_cleanup_expired() {
    if (!db) return 0;

    // Calculate cutoff time: 24 hours ago in milliseconds
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    long long cutoff_ms = now_ms - (48LL * 60 * 60 * 1000); // 48 hours ago to keep history for series detection

    char *sql = "DELETE FROM programs WHERE end_time < ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cleanup prepare error: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, cutoff_ms);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (deleted > 0) {
        printf("[DB] Cleaned up %d expired program entries\n", deleted);
    }
    return deleted;
}
