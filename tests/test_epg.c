#define _GNU_SOURCE
#include <assert.h>
#include <sys/stat.h>
#define EPG_SCAN_TIMEOUT_MS 1000
#include "../src/epg.c"
int g_verbose = 0;
int g_no_epg = 0;

static int reject_lease;
int __real_tuner_set_process(Tuner *, unsigned long, pid_t);
int __wrap_tuner_set_process(Tuner *t, unsigned long lease, pid_t pid) {
    if (!reject_lease) return __real_tuner_set_process(t, lease, pid);
    const char *record = getenv("EPG_RECORD");
    int ready = 0;
    for (int n = 0; n < 200 && !ready; n++) {
        FILE *f = fopen(record, "r");
        if (f) { int parent, child; ready = fscanf(f, "%d %d", &parent, &child) == 2; fclose(f); }
        if (!ready) usleep(10000);
    }
    assert(ready); /* child has installed its SIGTERM handler */
    return 0;
}

static int64_t test_now_ms(void) {
    struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void assert_stopped(int pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    for (int attempt = 0; attempt < 200; attempt++) {
        FILE *f = fopen(path, "r"); if (!f) return;
        char line[512];
        char *read = fgets(line, sizeof(line), f); fclose(f);
        if (!read) { usleep(1000); continue; } /* process may exit after fopen */
        const char *end = strrchr(line, ')'); assert(end);
        if (end[2] == 'Z' || end[2] == 'X') return;
        usleep(1000);
    }
    assert(0 && "EPG child or descendant survived cleanup");
}

static void test_index_merge(void) {
    ScanContext ctx = {.freq = "587000000"};
    ctx_update_description(&ctx, 101, 7, "seven");
    ctx_update_description(&ctx, 101, 9, "nine");
    ctx_update_description(&ctx, 101, 11, "eleven");
    Program p = {.source_id = 101, .event_id = 7};
    ctx_upsert(&ctx, &p);
    ctx_update_description(&ctx, 101, 11, "updated eleven");
    p.event_id = 11; ctx_upsert(&ctx, &p);
    p.event_id = 9; ctx_upsert(&ctx, &p);
    assert(ctx.pending_count == 0 && ctx.list.count == 3);
    assert(!strcmp(ctx.list.programs[0].description, "seven"));
    assert(!strcmp(ctx.list.programs[1].description, "updated eleven"));
    assert(!strcmp(ctx.list.programs[2].description, "nine"));
    p.event_id = 7 + EVENT_INDEX_SIZE; ctx_upsert(&ctx, &p); /* deliberate hash collision */
    assert(ctx.list.count == 4);
    ctx_update_description(&ctx, 101, p.event_id, "collision");
    assert(!strcmp(ctx.list.programs[0].description, "seven"));
    assert(!strcmp(ctx.list.programs[3].description, "collision"));
    ctx_upsert(&ctx, &p); assert(ctx.list.count == 4);
    ctx_free_buffers(&ctx);
}

static int mss(unsigned char *buf, const char *text) {
    int n = strlen(text);
    unsigned char header[] = {1, 'e', 'n', 'g', 1, 0, 0, n};
    memcpy(buf, header, sizeof(header)); memcpy(buf+8, text, n);
    return n+8;
}

static int finish(unsigned char *buf, int size) {
    int length = size+4-3;
    buf[1] = 0xf0 | (length >> 8); buf[2] = length;
    uint32_t crc = 0xffffffffU;
    for (int i = 0; i < size; i++) {
        crc ^= (uint32_t)buf[i] << 24;
        for (int bit = 0; bit < 8; bit++) crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
    }
    for (int i = 0; i < 4; i++) buf[size+i] = crc >> (24-i*8);
    return size+4;
}

static void test_ordering(int ett_first) {
    ScanContext ctx = {.freq = "587000000"};
    add_source_map(ctx.freq, 101, "15.1");
    unsigned char eit[128] = {0xcb}, ett[128] = {0xcc};
    eit[3] = 0; eit[4] = 101; eit[9] = 1;
    eit[10] = 0; eit[11] = 7;
    uint32_t gps = time(NULL) - 315964800 + 18;
    for (int i=0; i<4; i++) eit[12+i] = gps >> (24-8*i);
    eit[18] = 60;
    int title = mss(eit+20, "Example"); eit[19] = title;
    int eit_len = finish(eit, 22+title);
    uint32_t etm = (101U << 16) | (7U << 2) | 2;
    for (int i=0; i<4; i++) ett[9+i] = etm >> (24-8*i);
    int ett_len = finish(ett, 13+mss(ett+13, "Description"));
    assert(section_crc_valid(eit, eit_len));
    unsigned char mgt[64] = {0xc7};
    mgt[10] = 1; mgt[11] = 2; mgt[12] = 0; /* ETT-0 table type */
    mgt[13] = 0xe1; mgt[14] = 0x20;
    int mgt_len = finish(mgt, 24);
    handle_section(&ctx, 0x1ffb, mgt, mgt_len);
    assert(ctx.guide_pid_count == 1 && ctx.guide_pids[0] == 0x120);
    if (ett_first) handle_section(&ctx, 0x120, ett, ett_len);
    handle_section(&ctx, 0x1ffb, eit, eit_len);
    if (!ett_first) handle_section(&ctx, 0x120, ett, ett_len);
    assert(ctx.list.count == 1);
    assert(!strcmp(ctx.list.programs[0].frequency, "587000000"));
    assert(!strcmp(ctx.list.programs[0].description, "Description"));
    assert(!strcmp(ctx.list.programs[0].title, "Example"));
    assert(ctx.pending_count == 0);
    handle_section(&ctx, 0x1ffb, eit, eit_len); assert(ctx.list.count == 1);
    ett[ett_len-1] ^= 1; handle_section(&ctx, 0x1ffb, ett, ett_len);
    handle_section(&ctx, 0x1ffb, eit, 9); assert(ctx.list.count == 1);
    ctx_free_buffers(&ctx);
    ScanContext corrupt = {.freq = "587000000"};
    eit[eit_len-1] ^= 1;
    handle_section(&corrupt, 0x1ffb, eit, eit_len); assert(corrupt.list.count == 0);
    eit[eit_len-1] ^= 1;
    unsigned char packet[188]; memset(packet, 0xff, sizeof(packet));
    packet[0] = 0x47; packet[1] = 0x5f; packet[2] = 0xfb; packet[3] = 0x10;
    packet[4] = 0; memcpy(packet+5, eit, eit_len);
    parse_ts_chunk(&corrupt, packet, sizeof(packet)); assert(corrupt.list.count == 1);
    ctx_free_buffers(&corrupt);
}

static void test_limits(void) {
    ScanContext ctx = {.freq = "587000000"};
    for (int n=0; n<=MAX_PENDING_ETT; n++) ctx_update_description(&ctx, 101, n, "pending");
    assert(ctx.pending_count == MAX_PENDING_ETT);
    Program p = {.source_id = 102};
    for (int n=0; n<=MAX_SCAN_PROGRAMS; n++) { p.event_id = n; ctx_upsert(&ctx, &p); }
    assert(ctx.list.count == MAX_SCAN_PROGRAMS);
    assert(!ctx.list.programs[0].description[0]); /* source IDs must match */
    ctx_free_buffers(&ctx);
}

static void test_spawn(void) {
    char directory[] = "/tmp/zaplink-epg-spawn.XXXXXX"; assert(mkdtemp(directory));
    char path[4096], mock[512], record[512];
    snprintf(mock, sizeof(mock), "%s/dvbv5-zap", directory);
    snprintf(record, sizeof(record), "%s/record", directory);
    FILE *f = fopen(mock, "w"); assert(f);
    fputs("#!/bin/sh\n"
          "while [ $# -gt 0 ]; do case $1 in -c) config=$2; shift 2;; *) name=$1; shift;; esac; done\n"
          "[ ! -e /proc/$$/fd/$SENTINEL_FD ] || exit 5\n"
          "[ \"$name\" = zaplink-selected ] || exit 6\n"
          "[ \"$(grep -c '^FREQUENCY' \"$config\")\" = 1 ] || exit 7\n"
          "grep -q 'FREQUENCY = 587000000' \"$config\" || exit 8\n"
          "grep -q 'SERVICE_ID = 7' \"$config\" || exit 9\n"
          "if [ \"$EPG_MODE\" = normal ]; then printf OK > \"$EPG_RECORD\"; exit 0; fi\n"
          "case $EPG_MODE in signal|signal-bus)\n"
          "  ulimit -c 0; printf '%s %s' $$ $$ > \"$EPG_RECORD\"\n"
          "  if [ \"$EPG_MODE\" = signal ]; then kill -SEGV $$; else kill -BUS $$; fi;; esac\n"
          "trap '' TERM\n"
          "[ \"$EPG_MODE\" != closed ] || exec >/dev/null 2>&1\n"
          "sleep 30 &\n"
          "printf '%s %s' $$ $! > \"$EPG_RECORD\"\n"
          "[ \"$EPG_MODE\" != descendant ] || exit 0\n"
          "if [ \"$EPG_MODE\" = flood ]; then while :; do printf 'not transport data'; done; fi\n"
          "wait\n", f);
    assert(!fclose(f)); assert(!chmod(mock, 0700));
    char *saved = strdup(getenv("PATH")); assert(saved);
    snprintf(path, sizeof(path), "%s:%s", directory, saved); setenv("PATH", path, 1);
    setenv("EPG_RECORD", record, 1);
    int sentinel = open("/dev/null", O_RDONLY); assert(sentinel >= 0);
    char fd[16]; snprintf(fd, sizeof(fd), "%d", sentinel); setenv("SENTINEL_FD", fd, 1);
    tuner_count = 1; tuners[0].id = 0;
    unsigned long generation; Tuner *t = acquire_tuner(USER_EPG, &generation); assert(t);
    ScanContext ctx = {.freq = "587000000"};
    setenv("EPG_MODE", "normal", 1);
    scan_mux(t, generation, &ctx, "15.1", "MARKET-B");
    f = fopen(record, "r"); assert(f); assert(fgetc(f) == 'O'); fclose(f);
    unlink(record); reject_lease = 1; setenv("EPG_MODE", "hung", 1);
    int64_t started = test_now_ms();
    scan_mux(t, generation, &ctx, "15.1", "MARKET-B");
    assert(test_now_ms() - started < 4000);
    f = fopen(record, "r"); assert(f); int child; assert(fscanf(f, "%d", &child) == 1); fclose(f);
    assert(kill(child, 0) < 0 && errno == ESRCH);
    reject_lease = 0;
    const char *modes[] = {"hung", "closed", "descendant", "flood", "signal", "signal-bus"};
    atomic_store(&epg_running, 1);
    for (size_t n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        unlink(record); setenv("EPG_MODE", modes[n], 1);
        started = test_now_ms();
        scan_mux(t, generation, &ctx, "15.1", "MARKET-B");
        assert(test_now_ms() - started < 4000);
        f = fopen(record, "r"); assert(f); int descendant;
        assert(fscanf(f, "%d %d", &child, &descendant) == 2); fclose(f);
        assert_stopped(child); assert_stopped(descendant);
        assert(mux_queue_count == 0); /* failures and crashes must not cause an immediate retry loop */
        assert(t->zap_pid == 0 && tuner_lease_is_current(t, generation));
        release_tuner(t, generation);
        unsigned long stream_generation;
        Tuner *stream = acquire_tuner(USER_STREAM, &stream_generation); assert(stream == t);
        release_tuner(stream, stream_generation);
        t = acquire_tuner(USER_EPG, &generation); assert(t);
    }
    atomic_store(&epg_running, 0); unsetenv("EPG_MODE");
    release_tuner(t, generation); close(sentinel);
    setenv("PATH", saved, 1); free(saved); unsetenv("EPG_RECORD"); unsetenv("SENTINEL_FD");
    unlink(mock); unlink(record); assert(!rmdir(directory));
}

int main(void) {
    assert(load_channels("tests/fixtures/channels.conf") == 3);
    snprintf(channels_conf_path, sizeof(channels_conf_path), "tests/fixtures/channels.conf");
    test_ordering(1); test_ordering(0); test_limits(); test_index_merge(); test_spawn();
    puts("EPG ordering, bounds and spawn tests: OK");
}
