#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "db.h"

int g_verbose = 0;
int g_no_epg = 0;

static void *write_program(void *argument) {
    Program *program = argument;
    ProgramList list = {.programs = program, .count = 1, .capacity = 1};
    for (int i = 0; i < 10; i++) db_bulk_upsert(&list);
    return NULL;
}

int main(void) {
    char original_directory[4096];
    assert(getcwd(original_directory, sizeof(original_directory)) != NULL);
    char temp_directory[] = "/tmp/zaplink-db-test.XXXXXX";
    assert(mkdtemp(temp_directory) != NULL);
    assert(chdir(temp_directory) == 0);
    assert(db_init());

    long long start = (long long)time(NULL) * 1000LL + 60000LL;
    Program programs[2] = {0};
    snprintf(programs[0].frequency, sizeof(programs[0].frequency), "581000000");
    snprintf(programs[0].channel_service_id,
             sizeof(programs[0].channel_service_id), "15.1");
    snprintf(programs[0].title, sizeof(programs[0].title), "Writer A");
    programs[0].start_time = start;
    programs[0].end_time = start + 3600000LL;
    programs[0].event_id = 1;
    programs[0].source_id = 1;

    programs[1] = programs[0];
    snprintf(programs[1].frequency, sizeof(programs[1].frequency), "587000000");
    snprintf(programs[1].channel_service_id,
             sizeof(programs[1].channel_service_id), "15.2");
    snprintf(programs[1].title, sizeof(programs[1].title), "Writer B");
    programs[1].event_id = 2;
    programs[1].source_id = 2;

    pthread_t writers[2];
    assert(pthread_create(&writers[0], NULL, write_program, &programs[0]) == 0);
    assert(pthread_create(&writers[1], NULL, write_program, &programs[1]) == 0);
    assert(pthread_join(writers[0], NULL) == 0);
    assert(pthread_join(writers[1], NULL) == 0);

    char *json = db_get_json_programs();
    assert(json != NULL);
    assert(strstr(json, "Writer A") != NULL);
    assert(strstr(json, "Writer B") != NULL);
    free(json);
    db_close();

    assert(unlink("epg.db") == 0);
    assert(chdir(original_directory) == 0);
    assert(rmdir(temp_directory) == 0);
    return 0;
}
