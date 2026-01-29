/**
 * @file db.h
 * @brief SQLite database interface for EPG storage - Minimal Core
 */

#ifndef DB_H
#define DB_H

/**
 * Initialize the database connection and create tables if needed
 * @return 1 on success, 0 on failure
 */
int db_init();

/**
 * Close the database connection
 */
void db_close();

/**
 * Check if the database has any program data
 * Used to determine if first EPG scan can be skipped
 * @return 1 if data exists, 0 if empty
 */
int db_has_data();

/**
 * Generate XMLTV-formatted program guide
 * @return Allocated XML string (caller must free), or NULL on error
 */
char *db_get_xmltv_programs();

/**
 * Generate JSON-formatted program guide
 * @return Allocated JSON string (caller must free), or NULL on error
 */
char *db_get_json_programs();

/**
 * Insert or update a program entry
 */
void db_upsert_program(const char *frequency, const char *channel_service_id, 
                       long long start_time, long long end_time, 
                       const char *title, int event_id, int source_id);

/**
 * Update program description from ETT
 */
void db_update_program_description(const char *frequency, 
                                   const char *channel_service_id, 
                                   int event_id, const char *description);

/**
 * Delete program entries that ended more than 24 hours ago
 * @return Number of entries deleted
 */
// Delete expired programs
int db_cleanup_expired();

typedef struct {
    char frequency[32];
    char channel_service_id[32];
    long long start_time;
    long long end_time;
    char title[256];
    char description[1024];
    int event_id;
    int source_id;
} Program;

typedef struct {
    Program *programs;
    int count;
    int capacity;
} ProgramList;

// Batch operations
void db_bulk_upsert(ProgramList *list);
void db_invalidate_cache();

// Transaction control (still useful for manual batches if needed)
void db_begin_transaction();
void db_commit_transaction();

#endif /* DB_H */
