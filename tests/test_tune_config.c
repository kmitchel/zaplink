#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tune_config.h"

static void test_service_id_formats(void) {
    char source[] = "/tmp/zaplink-service-ids.XXXXXX";
    int fd = mkstemp(source); assert(fd >= 0);
    FILE *f = fdopen(fd, "w"); assert(f);
    fputs("[EIGHT]\nVCHANNEL = 15.1\nFREQUENCY = 581000000\nSERVICE_ID = 08\n"
          "[TEN]\nVCHANNEL = 15.1\nFREQUENCY = 581000000\nSERVICE_ID = 010\n"
          "[ELEVEN]\nVCHANNEL = 15.1\nFREQUENCY = 581000000\nSERVICE_ID = 0x0b\n", f);
    assert(!fclose(f));
    const char *requests[] = {"8", "10", "11", "08", "010", "0x0b"};
    const char *canonical[] = {"8", "10", "11", "8", "10", "11"};
    for (int i = 0; i < 6; i++) {
        char path[64], content[512], expected[64];
        assert(tune_config_create(source, "581000000", requests[i], path, sizeof(path)));
        f = fopen(path, "r"); assert(f);
        size_t n = fread(content, 1, sizeof(content)-1, f); content[n] = 0; fclose(f);
        snprintf(expected, sizeof(expected), "SERVICE_ID = %s\n", canonical[i]);
        assert(strstr(content, expected));
        assert(!strstr(content, "SERVICE_ID = 0"));
        assert(!strchr(strchr(content, ']') + 1, '['));
        unlink(path);
    }
    char path[64];
    assert(!tune_config_create(source, "581000000", "0x", path, sizeof(path)));
    assert(!tune_config_create(source, "581000000", "65536", path, sizeof(path)));
    unlink(source);
}

int main(void) {
    test_service_id_formats();
    char path[64], content[4096];
    assert(tune_config_create("tests/fixtures/channels.conf", "587000000", "7", path, sizeof(path)));
    struct stat info; assert(!stat(path, &info)); assert((info.st_mode & 0777) == 0600);
    FILE *file = fopen(path, "r"); assert(file);
    size_t n = fread(content, 1, sizeof(content)-1, file); content[n] = 0; fclose(file);
    assert(strstr(content, "[zaplink-selected]"));
    assert(strstr(content, "FREQUENCY = 587000000"));
    assert(strstr(content, "SERVICE_ID = 7"));
    assert(strstr(content, "MODULATION = VSB/8"));
    assert(!strstr(content, "581000000")); assert(!strstr(content, "593000000"));
    unlink(path);
    assert(!tune_config_create("tests/fixtures/channels.conf", "587000000", "3", path, sizeof(path)));
    assert(!path[0]);
    assert(tune_config_create("tests/fixtures/channels.conf", "581000000", NULL, path, sizeof(path)));
    unlink(path);
    puts("exact tuning configuration tests: OK");
}
