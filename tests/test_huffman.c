#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "huffman.h"

int g_verbose = 0;

static void test_invalid_parameters(void) {
    char out[128];
    uint8_t dummy[8] = {0};

    /* Invalid compression type (must be 1 or 2) */
    assert(huffman_decode(0, dummy, 4, out, sizeof(out)) == 0);
    assert(huffman_decode(3, dummy, 4, out, sizeof(out)) == 0);

    /* NULL buffers or zero lengths */
    assert(huffman_decode(1, NULL, 4, out, sizeof(out)) == 0);
    assert(huffman_decode(1, dummy, 0, out, sizeof(out)) == 0);
    assert(huffman_decode(1, dummy, 4, NULL, sizeof(out)) == 0);
    assert(huffman_decode(1, dummy, 4, out, 0) == 0);
}

static void test_initialization_and_cleanup(void) {
    /* Initialize tables from huffman.bin */
    int ok = huffman_init();
    assert(ok == 1);

    /* Test title decode with empty/short bitstream */
    char decoded[256];
    memset(decoded, 0, sizeof(decoded));
    uint8_t zero_byte[2] = {0x00, 0x00};

    /* Should handle zero-bits or termination cleanly */
    int res = huffman_decode(1, zero_byte, 1, decoded, sizeof(decoded));
    (void)res; /* Should not crash or overflow */

    /* Test description decode (type 2) */
    memset(decoded, 0, sizeof(decoded));
    res = huffman_decode(2, zero_byte, 1, decoded, sizeof(decoded));
    (void)res;

    huffman_cleanup();
}

int main(void) {
    test_invalid_parameters();
    test_initialization_and_cleanup();

    puts("huffman decoder tests: OK");
    return 0;
}
