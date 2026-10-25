#ifndef SERVICE_ID_H
#define SERVICE_ID_H
#include <errno.h>
#include <stdlib.h>
/* Leading zeroes are decimal. Only an explicit 0x prefix selects hex. */
static inline int parse_service_id(const char *text, unsigned int *result) {
    if (!text || !*text) return 0;
    int base = 10;
    const char *digits = text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; digits += 2; }
    if (!*digits) return 0;
    for (const char *p = digits; *p; p++) {
        if (*p >= '0' && *p <= '9') continue;
        if (base == 16 && ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) continue;
        return 0;
    }
    errno = 0;
    unsigned long value = strtoul(digits, NULL, base);
    if (errno || !value || value > 65535) return 0;
    if (result) *result = (unsigned int)value;
    return 1;
}
#endif
