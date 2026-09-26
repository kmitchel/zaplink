#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "huffman.h"

int g_verbose = 0;

typedef struct {
    char magic[4];
    uint32_t version, title_offset, desc_offset, nodes_per_tree;
} Header;

static void make_fixture(int placeholder) {
    Header header = {{'A', 'T', 'H', 'U'}, 1, sizeof(Header),
                     sizeof(Header) + 128 * 256 * sizeof(HuffmanNode), 256};
    FILE *file = fopen("huffman.bin", "wb");
    assert(file);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    for (int type = 0; type < 2; type++) {
        for (int context = 0; context < 128; context++) {
            for (int node = 0; node < 256; node++) {
                HuffmanNode value = {{-1, -1}};
                if (!placeholder && node == 0) {
                    value.children[0] = type == 0 ? -66 : -67;
                }
                assert(fwrite(&value, sizeof(value), 1, file) == 1);
            }
        }
    }
    assert(fclose(file) == 0);
}

static void copy_asset(const char *path) {
    FILE *source = fopen(path, "rb");
    FILE *dest = fopen("huffman.bin", "wb");
    assert(source && dest);
    unsigned char buffer[4096];
    size_t length;
    while ((length = fread(buffer, 1, sizeof(buffer), source)) != 0) {
        assert(fwrite(buffer, 1, length, dest) == length);
    }
    assert(!ferror(source));
    assert(fclose(source) == 0);
    assert(fclose(dest) == 0);
}

int main(void) {
    char output[32] = {0};
    unsigned char bits[] = {0x40}; /* 0 emits A/B, then 1 terminates. */
    assert(!huffman_decode(0, bits, 1, output, sizeof(output)));
    assert(!huffman_decode(1, NULL, 1, output, sizeof(output)));

    char original[4096], asset[sizeof(original) + 16];
    assert(getcwd(original, sizeof(original)));
    assert(snprintf(asset, sizeof(asset), "%s/huffman.bin", original) < (int)sizeof(asset));
    char directory[] = "/tmp/zaplink-huffman-test.XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);

    make_fixture(1);
    assert(!huffman_init());
    assert(!huffman_decode(1, bits, 1, output, sizeof(output)));
    huffman_cleanup();
    make_fixture(0);
    assert(huffman_init());
    assert(huffman_decode(1, bits, 1, output, sizeof(output)));
    assert(strcmp(output, "A") == 0);
    output[0] = '\0';
    assert(huffman_decode(2, bits, 1, output, sizeof(output)));
    assert(strcmp(output, "B") == 0);
    huffman_cleanup();

    copy_asset(asset);
    assert(huffman_init());
    /* A/65:2013 Figure F2: type 1 encoding of "The next" (39 bits). */
    const unsigned char title[] = {0x43, 0x28, 0xdc, 0x84, 0xd4};
    output[0] = '\0';
    assert(huffman_decode(1, title, sizeof(title), output, sizeof(output)));
    assert(strcmp(output, "The next") == 0);
    output[0] = '\0';
    assert(!huffman_decode(1, title, sizeof(title) - 1, output, sizeof(output)));
    /* Table C4: initial ESC 11001011, literal Latin-1 0xe9, then raw terminator. */
    const unsigned char latin1[] = {0xcb, 0xe9, 0x00};
    output[0] = '\0';
    assert(huffman_decode(1, latin1, sizeof(latin1), output, sizeof(output)));
    assert((unsigned char)output[0] == 0xe9 && output[1] == '\0');
    /* A/65 Table C6: 0->A:010, A->ESC:10010, literal '.':00101110, '.'->end:1. */
    const unsigned char desc[] = {0x52, 0x2e, 0x80};
    output[0] = '\0';
    assert(huffman_decode(2, desc, sizeof(desc), output, sizeof(output)));
    assert(strcmp(output, "A.") == 0);
    huffman_cleanup();
    assert(unlink("huffman.bin") == 0);
    assert(chdir(original) == 0);
    assert(rmdir(directory) == 0);
    puts("huffman table validation and ATSC vectors: OK");
    return 0;
}
