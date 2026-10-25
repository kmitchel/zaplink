#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "db.h"
#include "channels.h"
int g_verbose = 0;
int g_no_epg = 0;

int main(void) {
    char path[] = "/tmp/zaplink-xmltv.XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    setenv("ZAPLINK_DB_PATH", path, 1); assert(db_init());
    assert(load_channels("tests/fixtures/channels.conf") == 3); build_channel_lookup();
    Program p = {.start_time = 4102444800000LL, .end_time = 4102448400000LL, .event_id = 123};
    strcpy(p.frequency, "587000000"); strcpy(p.channel_service_id, "15.1");
    strcpy(p.title, "Repeated & <Title>"); strcpy(p.description, "A \"description\"");
    ProgramList list = {.programs=&p, .count=1, .capacity=1}; assert(db_bulk_upsert(&list));
    p.start_time += 3600000; p.end_time += 3600000; p.event_id++;
    assert(db_bulk_upsert(&list));
    char *xml = db_get_xmltv_programs(); assert(xml);
    const char *expected =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n<tv generator-info-name=\"ZapLink\">\n"
        "  <channel id=\"15.1-581000000-3\">\n    <display-name>MARKET-A</display-name>\n  </channel>\n"
        "  <channel id=\"15.1-587000000-7\">\n    <display-name>MARKET-B &amp; &quot;TV&quot;</display-name>\n  </channel>\n"
        "  <channel id=\"21.2\">\n    <display-name>UNIQUE</display-name>\n  </channel>\n"
        "  <programme start=\"21000101000000 +0000\" stop=\"21000101010000 +0000\" channel=\"15.1-587000000-7\">\n"
        "    <title>Repeated &amp; &lt;Title&gt;</title>\n    <desc>A &quot;description&quot;</desc>\n  </programme>\n"
        "  <programme start=\"21000101010000 +0000\" stop=\"21000101020000 +0000\" channel=\"15.1-587000000-7\">\n"
        "    <title>Repeated &amp; &lt;Title&gt;</title>\n    <desc>A &quot;description&quot;</desc>\n  </programme>\n</tv>";
    assert(!strcmp(xml, expected)); free(xml);
    strcpy(channels[1].unique_id, "15&\""); db_invalidate_cache();
    xml = db_get_xmltv_programs(); assert(xml);
    assert(strstr(xml, "<channel id=\"15&amp;&quot;\">"));
    assert(strstr(xml, "channel=\"15&amp;&quot;\">")); free(xml);
    db_close(); unlink(path); puts("XMLTV exact output tests: OK");
}
