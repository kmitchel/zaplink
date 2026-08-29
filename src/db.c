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

/* XMLTV Cache */
static char *g_xmltv_cache = NULL;
static time_t g_last_update_time = 0;
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

void build_channel_lookup();

int db_init() {
    const char *configured_path = getenv("ZAPLINK_DB_PATH");
    const char *database_path = configured_path && *configured_path
        ? configured_path : DB_PATH;
    int rc = sqlite3_open_v2(database_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc) {
        LOG_ERROR("DB", "Cannot open database: %s", sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_busy_timeout(db, 5000);
    
    // Create Table if not exists
    char *sql = "CREATE TABLE IF NOT EXISTS programs ("
                "frequency TEXT, "
                "channel_service_id TEXT, " // NOTE: Currently stores Virtual Channel Number (e.g. "15.1"), not DVB Service ID
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
        LOG_ERROR("DB", "Schema creation failed: %s", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }

    // Index for title to speed up series detection (counting occurrences)
    char *sql_idx = "CREATE INDEX IF NOT EXISTS idx_programs_title ON programs(title);"
                    "CREATE INDEX IF NOT EXISTS idx_programs_endtime ON programs(end_time);"
                    "CREATE INDEX IF NOT EXISTS idx_programs_channel ON programs(channel_service_id);";
    rc = sqlite3_exec(db, sql_idx, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("DB", "Index creation failed: %s", err_msg);
        sqlite3_free(err_msg);
    }
    
    return 1;
}

void db_close() {
    pthread_mutex_lock(&db_stmt_mutex);
    if (stmt_upsert) { sqlite3_finalize(stmt_upsert); stmt_upsert = NULL; }
    if (stmt_update_desc) { sqlite3_finalize(stmt_update_desc); stmt_update_desc = NULL; }
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
    pthread_mutex_unlock(&db_stmt_mutex);

    pthread_mutex_lock(&g_cache_mutex);
    if (g_xmltv_cache) { free(g_xmltv_cache); g_xmltv_cache = NULL; }
    pthread_mutex_unlock(&g_cache_mutex);

}

int db_has_data() {
    if (!db) return 0;
    pthread_mutex_lock(&db_stmt_mutex);
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
    pthread_mutex_unlock(&db_stmt_mutex);
    return has_data;
}

void db_invalidate_cache() {
    pthread_mutex_lock(&g_cache_mutex);
    g_last_update_time = 0; // Force regeneration
    pthread_mutex_unlock(&g_cache_mutex);
}

// Helper to append to dynamic string. Returns 1 on success, 0 on OOM.
int append_str(char **dest, size_t *size, size_t *cap, const char *src) {
    if (!dest || !*dest || !size || !cap || !src) return 0;
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



// Returns 0 on OOM, 1 on success.
// Batches runs of non-special characters to reduce per-character call overhead.
static int xml_escape_append(char **dest, size_t *size, size_t *cap, const char *src) {
    if (!src) return 1;
    const char *run_start = src;
    for (const char *p = src; ; p++) {
        const char *esc = NULL;
        if (*p == '&')       esc = "&amp;";
        else if (*p == '<')  esc = "&lt;";
        else if (*p == '>')  esc = "&gt;";
        else if (*p == '"')  esc = "&quot;";
        else if (*p == '\'') esc = "&apos;";
        else if (*p != '\0') continue;  /* ordinary character, extend the run */

        /* Flush any accumulated plain-text run */
        if (p > run_start) {
            size_t run_len = (size_t)(p - run_start);
            if (*size + run_len + 1 > *cap) {
                size_t new_cap = (*size + run_len + 1) * 2;
                if (new_cap < *cap + 1024 * 1024) new_cap = *cap + 1024 * 1024;
                char *tmp = realloc(*dest, new_cap);
                if (!tmp) return 0;
                *dest = tmp;
                *cap = new_cap;
            }
            memcpy(*dest + *size, run_start, run_len);
            *size += run_len;
            (*dest)[*size] = '\0';
        }

        if (*p == '\0') break;
        if (!append_str(dest, size, cap, esc)) return 0;
        run_start = p + 1;
    }
    return 1;
}

// Macro to bail on OOM
#define APPEND_OR_FAIL(expr) do { if (!(expr)) goto oom_fail; } while(0)

char *db_get_xmltv_programs() {
    if (!db) return NULL;
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_cache_mutex);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    
    // Return cache if valid (less than 5 mins old and not invalidated)
    if (g_xmltv_cache && (time(NULL) - g_last_update_time < 300)) {
        char *copy = strdup(g_xmltv_cache);
        pthread_mutex_unlock(&g_cache_mutex);
        return copy;
    }
    pthread_mutex_unlock(&g_cache_mutex);

    pthread_mutex_lock(&db_stmt_mutex);

    // Regenerate
    int row_count = 0;
    // Pre-count query
    sqlite3_stmt *count_stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM programs WHERE end_time > ?", -1, &count_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(count_stmt, 1, now_ms);
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
            row_count = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
    }
    
    size_t cap = (row_count > 0) ? (row_count * 600 + 4096) : (64 * 1024);
    size_t size = 0;
    char *xml = malloc(cap);
    if (!xml) {
        pthread_mutex_unlock(&db_stmt_mutex);
        return NULL;
    }
    xml[0] = '\0';

    APPEND_OR_FAIL(append_str(&xml, &size, &cap, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n<tv generator-info-name=\"ZapLink\">\n"));

    for (int i = 0; i < channel_count; i++) {
        char unique_id[128];
        get_unique_channel_id(&channels[i], unique_id, sizeof(unique_id));
        char buf[256];
        snprintf(buf, sizeof(buf), "  <channel id=\"%s\">\n    <display-name>", unique_id);
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, buf));
        APPEND_OR_FAIL(xml_escape_append(&xml, &size, &cap, channels[i].name));
        APPEND_OR_FAIL(append_str(&xml, &size, &cap, "</display-name>\n  </channel>\n"));
    }

    const char *sql = "SELECT title, description, start_time, end_time, channel_service_id, frequency, event_id, "
                      "(SELECT COUNT(*) FROM programs p2 WHERE p2.title = programs.title) as title_count "
                      "FROM programs "
                      "WHERE end_time > ? "
                      "ORDER BY CAST(SUBSTR(channel_service_id, 1, INSTR(channel_service_id, '.') - 1) AS INTEGER), "
                      "CAST(SUBSTR(channel_service_id, INSTR(channel_service_id, '.') + 1) AS INTEGER), start_time;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        free(xml);
        pthread_mutex_unlock(&db_stmt_mutex);
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
            ch = find_channel_fast(freq, svc_id);
        }
        char id_buf[128];
        const char *channel_id = "";
        if (ch) {
            channel_id = get_unique_channel_id(ch, id_buf, sizeof(id_buf));
        } else {
            channel_id = svc_id ? svc_id : "";
        }

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
    pthread_mutex_unlock(&db_stmt_mutex);

    // Update Cache
    pthread_mutex_lock(&g_cache_mutex);
    if (g_xmltv_cache) free(g_xmltv_cache);
    g_xmltv_cache = strdup(xml);
    g_last_update_time = time(NULL);
    pthread_mutex_unlock(&g_cache_mutex);

    return xml;

oom_fail:
    free(xml);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_stmt_mutex);
    LOG_ERROR("DB", "OOM while generating XMLTV");
    return NULL;
}

// Returns 0 on OOM, 1 on success.
// Batches runs of non-special characters to reduce per-character call overhead.
static int json_escape_append(char **dest, size_t *size, size_t *cap, const char *src) {
    if (!src) return 1;
    const char *run_start = src;
    for (const char *p = src; ; p++) {
        const char *esc = NULL;
        char esc_buf[8];
        if (*p == '"')        esc = "\\\"";
        else if (*p == '\\') esc = "\\\\";
        else if (*p == '\n')  esc = "\\n";
        else if (*p == '\r')  esc = "\\r";
        else if (*p == '\t')  esc = "\\t";
        else if ((unsigned char)*p < 0x20 && *p != '\0') {
            snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", (unsigned char)*p);
            esc = esc_buf;
        } else if (*p != '\0') continue;  /* ordinary character, extend the run */

        /* Flush any accumulated plain-text run */
        if (p > run_start) {
            size_t run_len = (size_t)(p - run_start);
            if (*size + run_len + 1 > *cap) {
                size_t new_cap = (*size + run_len + 1) * 2;
                if (new_cap < *cap + 1024 * 1024) new_cap = *cap + 1024 * 1024;
                char *tmp = realloc(*dest, new_cap);
                if (!tmp) return 0;
                *dest = tmp;
                *cap = new_cap;
            }
            memcpy(*dest + *size, run_start, run_len);
            *size += run_len;
            (*dest)[*size] = '\0';
        }

        if (*p == '\0') break;
        if (!append_str(dest, size, cap, esc)) return 0;
        run_start = p + 1;
    }
    return 1;
}

char *db_get_json_programs() {
    if (!db) return NULL;
    pthread_mutex_lock(&db_stmt_mutex);
    
    // (Optional: Implement JSON caching here too if needed, but skipped for now as per plan/task)

    const char *sql = "SELECT title, description, start_time, end_time, channel_service_id, frequency FROM programs "
                "WHERE end_time > ? "
                "ORDER BY CAST(channel_service_id AS INTEGER), "
                "CASE WHEN INSTR(channel_service_id, '.') > 0 THEN CAST(SUBSTR(channel_service_id, INSTR(channel_service_id, '.') + 1) AS INTEGER) ELSE 0 END, "
                "start_time";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    
    if (rc != SQLITE_OK) {
        LOG_ERROR("DB", "Failed to fetch data: %s", sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_stmt_mutex);
        return NULL;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    sqlite3_bind_int64(stmt, 1, now_ms);

    size_t cap = 1024 * 1024;
    size_t size = 0;
    char *json = malloc(cap);
    if (!json) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&db_stmt_mutex);
        return NULL;
    }
    json[0] = '\0';

    APPEND_OR_FAIL(append_str(&json, &size, &cap, "{\n  \"channels\": [\n"));

    for (int i = 0; i < channel_count; i++) {
        char buf[256];
        char channel_id[128];
        get_unique_channel_id(&channels[i], channel_id, sizeof(channel_id));
        snprintf(buf, sizeof(buf), "    {\"id\": \"%s\", \"name\": \"",
                 channel_id);
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
        const char *freq = (const char *)sqlite3_column_text(stmt, 5);
        char channel_id[128];
        Channel *channel = (freq && svc_id) ? find_channel_fast(freq, svc_id) : NULL;
        if (channel) get_unique_channel_id(channel, channel_id, sizeof(channel_id));
        else snprintf(channel_id, sizeof(channel_id), "%s", svc_id ? svc_id : "");

        if (!first) APPEND_OR_FAIL(append_str(&json, &size, &cap, ",\n"));
        first = 0;

        char buf[256];
        snprintf(buf, sizeof(buf), "    {\"channel\": \"%s\", \"start\": %lld, \"end\": %lld, \"title\": \"",
            channel_id, start, end);
        APPEND_OR_FAIL(append_str(&json, &size, &cap, buf));
        APPEND_OR_FAIL(json_escape_append(&json, &size, &cap, title));
        APPEND_OR_FAIL(append_str(&json, &size, &cap, "\", \"description\": \""));
        APPEND_OR_FAIL(json_escape_append(&json, &size, &cap, desc));
        APPEND_OR_FAIL(append_str(&json, &size, &cap, "\"}"));
    }

    APPEND_OR_FAIL(append_str(&json, &size, &cap, "\n  ]\n}"));
    
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_stmt_mutex);
    return json;

oom_fail:
    free(json);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_stmt_mutex);
    LOG_ERROR("DB", "OOM while generating JSON");
    return NULL;
}

static const char *SQL_PROGRAM_UPSERT =
    "INSERT INTO programs (frequency, channel_service_id, start_time, end_time, title, description, event_id, source_id) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(frequency, channel_service_id, start_time) "
    "DO UPDATE SET title=excluded.title, end_time=excluded.end_time, "
    "description=CASE WHEN excluded.description <> '' THEN excluded.description ELSE programs.description END, "
    "event_id=excluded.event_id, source_id=excluded.source_id;";

// Bulk Upsert Implementation
void db_bulk_upsert(ProgramList *list) {
    if (!db || !list || !list->programs || list->count == 0) return;

    pthread_mutex_lock(&db_stmt_mutex);
    char *transaction_error = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL,
                     &transaction_error) != SQLITE_OK) {
        LOG_ERROR("DB", "Failed to begin EPG transaction: %s",
                  transaction_error ? transaction_error : sqlite3_errmsg(db));
        sqlite3_free(transaction_error);
        pthread_mutex_unlock(&db_stmt_mutex);
        return;
    }
    int transaction_ok = 1;
    
    // Prepare statement if needed
    if (!stmt_upsert) {
        if (sqlite3_prepare_v2(db, SQL_PROGRAM_UPSERT, -1, &stmt_upsert, 0) != SQLITE_OK) {
            LOG_ERROR("DB", "Failed to prepare upsert stmt: %s", sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            pthread_mutex_unlock(&db_stmt_mutex);
            return;
        }
    }

    for (int i = 0; i < list->count; i++) {
        Program *p = &list->programs[i];
        
        sqlite3_bind_text(stmt_upsert, 1, p->frequency, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_upsert, 2, p->channel_service_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt_upsert, 3, p->start_time);
        sqlite3_bind_int64(stmt_upsert, 4, p->end_time);
        sqlite3_bind_text(stmt_upsert, 5, p->title, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_upsert, 6, p->description, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt_upsert, 7, p->event_id);
        sqlite3_bind_int(stmt_upsert, 8, p->source_id);

        if (sqlite3_step(stmt_upsert) != SQLITE_DONE) {
            LOG_ERROR("DB", "Upsert step failed: %s", sqlite3_errmsg(db));
            transaction_ok = 0;
        }
        
        sqlite3_reset(stmt_upsert);
        sqlite3_clear_bindings(stmt_upsert);
    }
    
    const char *finish_sql = transaction_ok ? "COMMIT;" : "ROLLBACK;";
    if (sqlite3_exec(db, finish_sql, NULL, NULL, &transaction_error) != SQLITE_OK) {
        LOG_ERROR("DB", "Failed to finish EPG transaction: %s",
                  transaction_error ? transaction_error : sqlite3_errmsg(db));
        transaction_ok = 0;
    }
    sqlite3_free(transaction_error);
    pthread_mutex_unlock(&db_stmt_mutex);
    
    // Invalidate cache after update
    if (transaction_ok) db_invalidate_cache();
}

void db_upsert_program(const char *frequency, const char *channel_service_id,
                       long long start_time, long long end_time,
                       const char *title, int event_id, int source_id) {
    if (!db || !frequency || !channel_service_id || !title) return;

    Program p;
    memset(&p, 0, sizeof(p));
    snprintf(p.frequency, sizeof(p.frequency), "%s", frequency);
    snprintf(p.channel_service_id, sizeof(p.channel_service_id), "%s", channel_service_id);
    p.start_time = start_time;
    p.end_time = end_time;
    snprintf(p.title, sizeof(p.title), "%s", title);
    p.description[0] = '\0';
    p.event_id = event_id;
    p.source_id = source_id;

    ProgramList list = { .programs = &p, .count = 1, .capacity = 1 };
    db_bulk_upsert(&list);
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

    sqlite3_step(stmt_update_desc);
    sqlite3_reset(stmt_update_desc);
    sqlite3_clear_bindings(stmt_update_desc);

    pthread_mutex_unlock(&db_stmt_mutex);
    db_invalidate_cache();
}

// Delete program entries that ended more than 24 hours ago
int db_cleanup_expired() {
    if (!db) return 0;
    pthread_mutex_lock(&db_stmt_mutex);

    // Calculate cutoff time: 48 hours ago in milliseconds (preserves history for series detection)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    long long cutoff_ms = now_ms - (48LL * 60 * 60 * 1000);

    char *sql = "DELETE FROM programs WHERE end_time < ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        LOG_ERROR("DB", "Cleanup prepare failed: %s", sqlite3_errmsg(db));
        pthread_mutex_unlock(&db_stmt_mutex);
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, cutoff_ms);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_stmt_mutex);

    if (deleted > 0) {
        LOG_INFO("DB", "Cleaned up %d expired program entries", deleted);
        db_invalidate_cache();
    }
    return deleted;
}
