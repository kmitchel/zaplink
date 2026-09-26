#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the real merge and worker-status logic without DVB hardware. */
#include "../src/scanner.c"

int g_verbose = 0;

static void run_case(const char *mode, int expected) {
    char directory[] = "/tmp/zaplink-scanner-test.XXXXXX";
    assert(mkdtemp(directory));
    char mock[256], candidate[256], part_files[MAX_ADAPTERS][128] = {{0}};
    snprintf(mock, sizeof(mock), "%s/dvbv5-scan", directory);
    snprintf(candidate, sizeof(candidate), "%s/candidate", directory);
    FILE *script = fopen(mock, "w");
    assert(script);
    fputs("#!/bin/sh\n"
          "while [ \"$#\" -gt 0 ]; do\n"
          "  case \"$1\" in\n"
          "    -a) adapter=$2; shift 2;;\n"
          "    -o) output=$2; shift 2;;\n"
          "    *) shift;;\n"
          "  esac\n"
          "done\n"
          "case \"$SCAN_MODE:$adapter\" in\n"
          "  partial:1|failed:0|failed:1) exit 1;;\n"
          "  launch-error:1) exit 127;;\n"
          "  empty:0|empty:1|mixed-empty:1) : > \"$output\"; exit 0;;\n"
          "esac\n"
          "printf '[STATION]\\nVCHANNEL = 15.1\\nSERVICE_ID = 3\\nFREQUENCY = 581000000\\n' > \"$output\"\n",
          script);
    assert(fclose(script) == 0);
    assert(chmod(mock, 0700) == 0);
    char *old_path = getenv("PATH");
    char path[2048];
    assert(snprintf(path, sizeof(path), "%s:%s", directory,
                    old_path ? old_path : "") < (int)sizeof(path));
    assert(setenv("PATH", path, 1) == 0);
    assert(setenv("SCAN_MODE", mode, 1) == 0);
    for (int i = 0; i < 2; i++) {
        snprintf(part_files[i], sizeof(part_files[i]), "%s/in%d", directory, i);
        FILE *part = fopen(part_files[i], "w");
        assert(part);
        fputs("[FREQUENCY]\nFREQUENCY = 581000000\n", part);
        assert(fclose(part) == 0);
    }
    assert(run_parallel_scan(2, part_files, candidate) == expected);
    assert((access(candidate, F_OK) == 0) == expected);
    if (expected) unlink(candidate);
    assert(setenv("PATH", old_path ? old_path : "", 1) == 0);
    unsetenv("SCAN_MODE");
    unlink(mock);
    assert(rmdir(directory) == 0);
}

int main(void) {
    run_case("partial", 0);
    run_case("failed", 0);
    run_case("launch-error", 0);
    run_case("empty", 0);
    run_case("mixed-empty", 1);
    run_case("success", 1);
    puts("scanner worker status tests: OK");
    return 0;
}
