#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include "tune_config.h"
#include "service_id.h"

static char *trim_value(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

int tune_config_create(const char *source, const char *frequency,
                       const char *service_id, char *path, size_t size) {
    if (!source || !frequency || !path || size < sizeof("/tmp/zaplink-tune.XXXXXX")) return 0;
    path[0] = 0;
    FILE *input = fopen(source, "re");
    if (!input) return 0;
    char *line = NULL;
    size_t capacity = 0;
    long start = -1, end = -1, next = 0;
    char freq[64] = "", sid[64] = "";
    int found = 0;
    unsigned int requested_service = 0, selected_service = 0;
    if (service_id && !parse_service_id(service_id, &requested_service)) {
        fclose(input); return 0;
    }
    for (;;) {
        long position = ftell(input);
        ssize_t length = getline(&line, &capacity, input);
        char *text = length >= 0 ? trim_value(line) : NULL;
        int section = text && text[0] == '[' && text[strlen(text)-1] == ']';
        if (length < 0 || section) {
            if (start >= 0 && !strcmp(freq, frequency) &&
                parse_service_id(sid, &selected_service) &&
                (!service_id || selected_service == requested_service)) {
                end = position;
                found = 1;
                break;
            }
            if (length < 0) break;
            start = ftell(input);
            freq[0] = sid[0] = 0;
        } else if (start >= 0 && text[0] != '#' && text[0] != ';') {
            char *equals = strchr(text, '=');
            if (equals) {
                *equals = 0;
                char *key = trim_value(text), *value = trim_value(equals + 1);
                if (!strcmp(key, "FREQUENCY")) snprintf(freq, sizeof(freq), "%s", value);
                if (!strcmp(key, "SERVICE_ID")) snprintf(sid, sizeof(sid), "%s", value);
            }
        }
    }
    int ok = found && !ferror(input) && fseek(input, start, SEEK_SET) == 0;
    FILE *output = NULL;
    if (ok) {
        snprintf(path, size, "/tmp/zaplink-tune.XXXXXX");
        int fd = mkostemp(path, O_CLOEXEC);
        if (fd >= 0) {
            output = fdopen(fd, "w");
            if (!output) close(fd);
        } else {
            path[0] = 0;
        }
        ok = output != NULL;
    }
    if (ok) {
        ok = fprintf(output, "[%s]\n", TUNE_SECTION) > 0;
        while (ok && (next = ftell(input)) >= 0 && next < end &&
               getline(&line, &capacity, input) >= 0) {
            const char *key = line;
            while (isspace((unsigned char)*key)) key++;
            if (!strncmp(key, "SERVICE_ID", 10)) {
                const char *equals = key + 10;
                while (isspace((unsigned char)*equals)) equals++;
                if (*equals == '=') {
                    ok = fprintf(output, "SERVICE_ID = %u\n", selected_service) > 0;
                    continue;
                }
            }
            ok = fputs(line, output) >= 0;
        }
        ok = ok && !ferror(input) && ftell(input) == end;
    }
    free(line);
    fclose(input);
    if (output && fclose(output) != 0) ok = 0;
    if (!ok && path[0]) { unlink(path); path[0] = 0; }
    return ok;
}
