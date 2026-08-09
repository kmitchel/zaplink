#include <assert.h>
#include <string.h>

#include "tuner.h"

int g_verbose = 0;

int main(void) {
    memset(tuners, 0, sizeof(Tuner) * MAX_TUNERS);
    tuner_count = 1;
    tuners[0].id = 0;

    unsigned long epg_generation = 0;
    Tuner *epg = acquire_tuner(USER_EPG, &epg_generation);
    assert(epg == &tuners[0]);
    assert(epg_generation != 0);

    unsigned long stream_generation = 0;
    Tuner *stream = acquire_tuner(USER_STREAM, &stream_generation);
    assert(stream == epg);
    assert(stream_generation != epg_generation);
    assert(stream->user_type == USER_STREAM);

    release_tuner(epg, epg_generation);
    assert(stream->in_use == 1);
    assert(stream->user_type == USER_STREAM);
    assert(tuner_lease_is_current(stream, stream_generation));

    release_tuner(stream, stream_generation);
    assert(stream->in_use == 0);
    assert(stream->user_type == USER_NONE);
    return 0;
}
