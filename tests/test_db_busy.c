#include <assert.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "db.h"

int g_verbose = 0;
int g_no_epg = 0;
extern sqlite3 *db;

static void *release_lock(void *opaque) {
    usleep(100000);
    assert(sqlite3_exec(opaque, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK);
    return NULL;
}

int main(void) {
    char path[] = "/tmp/zaplink-db-busy.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(setenv("ZAPLINK_DB_PATH", path, 1) == 0);
    assert(db_init());
    assert(sqlite3_busy_timeout(db, 20) == SQLITE_OK);

    sqlite3 *reader = NULL;
    assert(sqlite3_open(path, &reader) == SQLITE_OK);
    assert(sqlite3_exec(reader, "BEGIN; SELECT * FROM programs;", NULL, NULL, NULL) == SQLITE_OK);
    assert(!db_upsert_program("581000000", "15.1", 1000, 2000, "blocked", 1, 1));
    assert(sqlite3_get_autocommit(db));
    assert(sqlite3_exec(reader, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK);

    assert(db_upsert_program("581000000", "15.1", 3000, 4000, "persisted", 2, 1));
    assert(sqlite3_exec(reader, "BEGIN EXCLUSIVE;", NULL, NULL, NULL) == SQLITE_OK);
    pthread_t unlocker;
    assert(!pthread_create(&unlocker, NULL, release_lock, reader));
    db_update_program_description("581000000", "15.1", 2, "retried description");
    assert(!pthread_join(unlocker, NULL));
    sqlite3_stmt *check;
    assert(sqlite3_prepare_v2(db, "SELECT description FROM programs", -1, &check, NULL) == SQLITE_OK);
    assert(sqlite3_step(check) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(check, 0), "retried description"));
    sqlite3_finalize(check);
    assert(sqlite3_exec(reader, "BEGIN EXCLUSIVE;", NULL, NULL, NULL) == SQLITE_OK);
    assert(!pthread_create(&unlocker, NULL, release_lock, reader));
    assert(db_upsert_program("581000000", "15.1", 3000, 4000, "persisted", 2, 1));
    assert(!pthread_join(unlocker, NULL));
    /* Cleanup must survive a transient lock, as upserts and descriptions do. */
    assert(db_cleanup_expired() == 1);
    assert(db_upsert_program("581000000", "15.1", 3000, 4000, "persisted", 2, 1));
    assert(sqlite3_exec(reader, "BEGIN EXCLUSIVE;", NULL, NULL, NULL) == SQLITE_OK);
    assert(!pthread_create(&unlocker, NULL, release_lock, reader));
    assert(db_cleanup_expired() == 1);
    assert(!pthread_join(unlocker, NULL));
    assert(db_upsert_program("581000000", "15.1", 3000, 4000, "persisted", 2, 1));
    db_close();
    sqlite3_close(reader);
    assert(sqlite3_open(path, &reader) == SQLITE_OK);
    sqlite3_stmt *query = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT title FROM programs", -1, &query, NULL) == SQLITE_OK);
    assert(sqlite3_step(query) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(query, 0), "persisted") == 0);
    assert(sqlite3_step(query) == SQLITE_DONE);
    sqlite3_finalize(query);
    sqlite3_close(reader);
    unlink(path);
    puts("database busy recovery tests: OK");
    return 0;
}
