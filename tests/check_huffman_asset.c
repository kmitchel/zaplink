#include <stdio.h>
#include "huffman.h"
int g_verbose = 0;
int main(void) {
    if (!huffman_init()) {
        fputs("Cannot install: huffman.bin is missing, a placeholder, or invalid.\n", stderr);
        return 1;
    }
    huffman_cleanup();
    return 0;
}
