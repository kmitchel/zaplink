#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "db.h"

int g_verbose = 0;
int g_no_epg = 0;
extern sqlite3 *db;

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
