/* Linked only into the HTTP test server. Never installed. */
#include <string.h>
#include "tuner.h"
int __wrap_wait_for_tuners(unsigned int timeout) { (void)timeout; return 1; }
void __wrap_discover_tuners(void) {
    memset(tuners, 0, sizeof(Tuner) * MAX_TUNERS);
    tuner_count = 4;
    for (int n = 0; n < tuner_count; n++) tuners[n].id = n;
}
