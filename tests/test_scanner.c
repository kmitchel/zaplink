#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the real merge and worker-status logic without DVB hardware. */
#define SIGNAL_TIMEOUT_MS 1000
#define FETCH_TIMEOUT_MS 1000
#define SCAN_TIMEOUT_MS 1000
#include "../src/scanner.c"

int g_verbose = 0;

static void assert_process_stopped(int pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    for (int attempt = 0; attempt < 100; attempt++) {
        FILE *f = fopen(path, "r");
        if (!f) return;
        char line[512];
        char *read = fgets(line, sizeof(line), f); fclose(f);
        if (!read) { usleep(1000); continue; } /* process may exit after fopen */
        const char *end = strrchr(line, ')'); assert(end);
        if (end[2] == 'Z' || end[2] == 'X') return;
        usleep(1000);
    }
    assert(0 && "scanner child or descendant survived cleanup");
}

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
          "  sparse:1|sparse:7|sparse-single:1) :;;\n"
          "  sparse:*|sparse-single:*) exit 2;;\n"
          "  hang:*|closed-pipe:*|inherited-pipe:*)\n"
          "    [ \"$SCAN_MODE\" != closed-pipe ] || exec >/dev/null 2>&1\n"
          "    trap '' TERM; sleep 30 &\n"
          "    printf '%s %s' $$ $! > \"$output.pids\"\n"
          "    [ \"$SCAN_MODE\" != inherited-pipe ] || exit 0\n"
          "    wait;;\n"
          "  partial:1|failed:0|failed:1) exit 1;;\n"
          "  launch-error:1) exit 127;;\n"
          "  empty:0|empty:1|mixed-empty:1) : > \"$output\"; exit 0;;\n"
          "esac\n"
          "printf '[STATION]\\nVCHANNEL = 15.1\\nSERVICE_ID = 3\\nFREQUENCY = 581000000\\n' > \"$output\"\n",
          script);
    assert(fclose(script) == 0);
    assert(chmod(mock, 0700) == 0);
    char *old_path = strdup(getenv("PATH") ? getenv("PATH") : "");
    assert(old_path);
    char path[2048];
    assert(snprintf(path, sizeof(path), "%s:%s", directory,
                    old_path ? old_path : "") < (int)sizeof(path));
    assert(setenv("PATH", path, 1) == 0);
    assert(setenv("SCAN_MODE", mode, 1) == 0);
    int num_adapters = !strcmp(mode, "sparse-single") ? 1 : 2;
    int ids[MAX_ADAPTERS] = {0, 1};
    char adapter1[256], adapter7[256], adapter_bad[256];
    if (!strncmp(mode, "sparse", 6)) {
        snprintf(adapter1, sizeof(adapter1), "%s/adapter1", directory);
        snprintf(adapter7, sizeof(adapter7), "%s/adapter7", directory);
        snprintf(adapter_bad, sizeof(adapter_bad), "%s/adapter1junk", directory);
        assert(!mkdir(adapter1, 0700)); assert(!mkdir(adapter_bad, 0700));
        if (num_adapters == 2) assert(!mkdir(adapter7, 0700));
        char pattern[256]; snprintf(pattern, sizeof(pattern), "%s/adapter*", directory);
        assert(discover_scan_adapters(pattern, ids, MAX_ADAPTERS) == num_adapters);
        assert(ids[0] == 1);
        if (num_adapters == 2) assert(ids[1] == 7);
    }
    for (int i = 0; i < num_adapters; i++) {
        snprintf(part_files[i], sizeof(part_files[i]), "%s/in%d", directory, i);
        FILE *part = fopen(part_files[i], "w");
        assert(part);
        fputs("[FREQUENCY]\nFREQUENCY = 581000000\n", part);
        assert(fclose(part) == 0);
    }
    if (!strcmp(mode, "missing")) { unlink(mock); assert(!setenv("PATH", directory, 1)); }
    int64_t started = signal_now_ms();
    assert(run_parallel_scan(ids, num_adapters, part_files, candidate) == expected);
    assert(signal_now_ms() - started < 4000);
    assert((access(candidate, F_OK) == 0) == expected);
    if (expected) unlink(candidate);
    for (int i = 0; i < num_adapters; i++) {
        char record[256]; snprintf(record, sizeof(record), "%s.out.pids", part_files[i]);
        FILE *f = fopen(record, "r");
        if (f) {
            int parent, descendant; assert(fscanf(f, "%d %d", &parent, &descendant) == 2); fclose(f);
            assert_process_stopped(parent); assert_process_stopped(descendant); unlink(record);
        }
    }
    int status; assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(setenv("PATH", old_path ? old_path : "", 1) == 0);
    free(old_path);
    unsetenv("SCAN_MODE");
    unlink(mock);
    if (!strncmp(mode, "sparse", 6)) {
        rmdir(adapter1); rmdir(adapter_bad);
        if (num_adapters == 2) rmdir(adapter7);
    }
    assert(rmdir(directory) == 0);
}

static void test_signal_deadline(void) {
    char directory[] = "/tmp/zaplink-signal-test.XXXXXX";
    assert(mkdtemp(directory));
    char mock[256];
    snprintf(mock, sizeof(mock), "%s/dvbv5-zap", directory);
    FILE *file = fopen(mock, "w"); assert(file);
    fputs("#!/bin/sh\n"
          "while [ $# -gt 0 ]; do case $1 in -c) config=$2; shift 2;; *) name=$1; shift;; esac; done\n"
          "[ \"$name\" = zaplink-selected ] || exit 2\n"
          "[ \"$(grep -c '^FREQUENCY' \"$config\")\" = 1 ] || exit 3\n"
          "grep -q 'FREQUENCY = 587000000' \"$config\" || exit 4\n"
          "case $SIGNAL_MODE in\n"
          " good) printf 'Lock Signal= -40 C/N= 30\\n';;\n"
          " bad-exit) printf 'Lock Signal= -40 C/N= 30\\n'; exit 1;;\n"
          " hang) trap '' TERM; sleep 30 & wait;;\n"
          " silent) exit 0;;\n"
          "esac\n", file);
    assert(!fclose(file)); assert(!chmod(mock, 0700));
    char *old_path = strdup(getenv("PATH")); assert(old_path);
    char path[4096]; snprintf(path, sizeof(path), "%s:%s", directory, old_path);
    assert(!setenv("PATH", path, 1));
    ScanMultiplex mux = {0}; strcpy(mux.frequency, "587000000");
    const char *modes[] = {"good", "silent", "bad-exit", "hang"};
    for (int n = 0; n < 4; n++) {
        assert(!setenv("SIGNAL_MODE", modes[n], 1));
        int64_t start = signal_now_ms();
        assert(measure_multiplex("tests/fixtures/channels.conf", 0, &mux) == (n == 0));
        assert(mux.weak == (n != 0));
        assert(signal_now_ms() - start < 4000);
        if (n == 0) assert(mux.cnr_db == 30);
    }
    assert(!unlink(mock));
    assert(!setenv("PATH", directory, 1));
    assert(!measure_multiplex("tests/fixtures/channels.conf", 0, &mux));
    assert(mux.weak);
    assert(!setenv("PATH", old_path, 1)); free(old_path);
    unsetenv("SIGNAL_MODE"); assert(!rmdir(directory));
}

static void test_fetch_deadline(void) {
    char directory[] = "/tmp/zaplink-fetch-test.XXXXXX"; assert(mkdtemp(directory));
    char mock[256], scan[256], html[256], path[4096];
    snprintf(mock, sizeof(mock), "%s/curl", directory);
    snprintf(scan, sizeof(scan), "%s/scan", directory);
    snprintf(html, sizeof(html), "%s/html", directory);
    FILE *f = fopen(mock, "w"); assert(f);
    fputs("#!/bin/sh\n"
          "while [ $# -gt 0 ]; do case $1 in -o) output=$2; shift 2;; *) shift;; esac; done\n"
          "printf '(14)' > \"$output\"\n"
          "case $FETCH_MODE in good) exit 0;; failed) exit 22;;\n"
          "hang) trap '' TERM; sleep 30 & wait;; esac\n", f);
    assert(!fclose(f)); assert(!chmod(mock, 0700));
    char *saved = strdup(getenv("PATH")); assert(saved);
    snprintf(path, sizeof(path), "%s:%s", directory, saved); assert(!setenv("PATH", path, 1));
    const char *modes[] = {"good", "failed", "hang", "missing"};
    for (int i = 0; i < 4; i++) {
        if (i == 3) { unlink(mock); assert(!setenv("PATH", directory, 1)); }
        assert(!setenv("FETCH_MODE", modes[i], 1));
        f = fopen(html, "w"); assert(f); fputs("(15)", f); fclose(f); /* stale response */
        int64_t started = signal_now_ms();
        assert(fetch_and_process("12345", scan, html, 0));
        assert(signal_now_ms() - started < 4000);
        f = fopen(scan, "r"); assert(f); int count = 0; char line[512];
        while (fgets(line, sizeof(line), f)) if (line[0] == '[') count++;
        fclose(f); assert(count == (i == 0 ? 1 : 35));
        assert(access(html, F_OK) < 0); unlink(scan);
    }
    assert(!setenv("PATH", saved, 1)); free(saved); unsetenv("FETCH_MODE");
    assert(!rmdir(directory));
}

int main(void) {
    test_signal_deadline();
    test_fetch_deadline();
    run_case("partial", 0);
    run_case("failed", 0);
    run_case("launch-error", 0);
    run_case("missing", 0);
    run_case("empty", 0);
    run_case("mixed-empty", 1);
    run_case("success", 1);
    run_case("sparse", 1);
    run_case("sparse-single", 1);
    run_case("hang", 0);
    run_case("closed-pipe", 0);
    run_case("inherited-pipe", 0);
    puts("scanner worker status tests: OK");
    return 0;
}
