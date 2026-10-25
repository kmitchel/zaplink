#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channels.h"
int g_verbose = 0;

static void write_fixture(FILE *file) {
    fputs("[MARKET-A]\n"
          "\tVCHANNEL = 15.1\n"
          "\tSERVICE_ID = 08\n"
          "\tFREQUENCY = 581000000\n\n"
          "[MARKET-B]\n"
          "\tVCHANNEL = 15.1\n"
          "\tSERVICE_ID = 010\n"
          "\tFREQUENCY = 587000000\n\n"
          "[UNIQUE]\n"
          "\tVCHANNEL = 21.2\n"
          "\tSERVICE_ID = 0x0b\n"
          "\tFREQUENCY = 593000000\n"
          "[INVALID-VCN]\nVCHANNEL = 22.1\" injected=\"value\nSERVICE_ID = 10\nFREQUENCY = 593000000\n"
          "[INVALID-FREQUENCY]\nVCHANNEL = 23.1\nSERVICE_ID = 11\nFREQUENCY = 593000000\"\n"
          "[INVALID-SERVICE]\nVCHANNEL = 24.1\nSERVICE_ID = 12\"\nFREQUENCY = 593000000\n",
          file);
}

int main(void) {
    char path[] = "/tmp/zaplink-channels-test.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file != NULL);
    write_fixture(file);
    assert(fclose(file) == 0);

    assert(load_channels(path) == 3);
    unlink(path);
    build_channel_lookup();

    Channel *first = find_channel_by_frequency_number("581000000", "15.1");
    Channel *second = find_channel_by_frequency_number("587000000", "15.1");
    assert(first != NULL && second != NULL && first != second);
    assert(!strcmp(first->service_id, "8"));
    assert(!strcmp(second->service_id, "10"));
    assert(find_channel_by_freq_sid("581000000", 8) == first);
    assert(find_channel_by_freq_sid("587000000", 10) == second);
    assert(!find_channel_by_freq_sid("587000000", 8));

    char first_id[128];
    char second_id[128];
    get_unique_channel_id(first, first_id, sizeof(first_id));
    get_unique_channel_id(second, second_id, sizeof(second_id));
    assert(strcmp(first_id, second_id) != 0);
    assert(find_channel_by_id(first_id) == first);
    assert(find_channel_by_id(second_id) == second);
    assert(find_channel_by_id("15.1") == NULL);
    assert(find_channel_by_number("15.1") == NULL);

    Channel *unique = find_channel_by_id("21.2");
    assert(unique != NULL);
    assert(strcmp(unique->frequency, "593000000") == 0);
    assert(!strcmp(unique->service_id, "11"));
    assert(find_channel_by_freq_sid("593000000", 11) == unique);

    puts("channel identity tests: OK");
    return 0;
}
