#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include "huffman.h"
#include "log.h"

#define HUFFMAN_MAGIC "ATHU"
#define HUFFMAN_VERSION 1

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t title_offset;
    uint32_t desc_offset;
    uint32_t nodes_per_tree;
} HuffmanHeader;

static HuffmanNode *title_trees = NULL;
static HuffmanNode *desc_trees = NULL;
static int nodes_per_tree = 0;
static int huffman_initialized = 0;
static pthread_mutex_t huffman_mutex = PTHREAD_MUTEX_INITIALIZER;

int huffman_init() {
    int fd = open("huffman.bin", O_RDONLY);
    if (fd < 0) {
        // Not a fatal error, but decoding won't work
        LOG_DEBUG("HUFFMAN", "huffman.bin not found, Huffman decoding disabled");
        return 0;
    }

    struct stat st;
    fstat(fd, &st);

    HuffmanHeader header;
    if (read(fd, &header, sizeof(header)) != sizeof(header)) {
        close(fd);
        return 0;
    }

    if (memcmp(header.magic, HUFFMAN_MAGIC, 4) != 0 || header.version != HUFFMAN_VERSION) {
        LOG_WARN("HUFFMAN", "Invalid huffman.bin format or version");
        close(fd);
        return 0;
    }

    nodes_per_tree = header.nodes_per_tree;
    size_t tree_size = 128 * nodes_per_tree * sizeof(HuffmanNode);

    title_trees = malloc(tree_size);
    desc_trees = malloc(tree_size);

    if (!title_trees || !desc_trees) {
        perror("malloc");
        close(fd);
        return 0;
    }

    lseek(fd, header.title_offset, SEEK_SET);
    read(fd, title_trees, tree_size);

    lseek(fd, header.desc_offset, SEEK_SET);
    read(fd, desc_trees, tree_size);

    close(fd);
    LOG_DEBUG("HUFFMAN", "Tables loaded (%d nodes per tree)", nodes_per_tree);
    return 1;
}

void huffman_cleanup() {
    if (title_trees) free(title_trees);
    if (desc_trees) free(desc_trees);
    title_trees = NULL;
    desc_trees = NULL;
}

static int get_bit(const uint8_t *src, int bit_pos) {
    return (src[bit_pos / 8] >> (7 - (bit_pos % 8))) & 1;
}

int huffman_decode(int compr_type, const uint8_t *src, int src_len, char *dest, int dest_len) {
    // Lazy initialization: load tables on first decode attempt
    pthread_mutex_lock(&huffman_mutex);
    if (!huffman_initialized) {
        huffman_init();
        huffman_initialized = 1;
    }
    pthread_mutex_unlock(&huffman_mutex);

    HuffmanNode *base_trees = (compr_type == 1) ? title_trees : desc_trees;
    if (!base_trees) return 0;

    int bit_pos = 0;
    int max_bits = src_len * 8;
    int dest_pos = (int)strlen(dest);
    int prev_char = 0; // Standard says initial context is 0x00

    while (bit_pos < max_bits && dest_pos < dest_len - 1) {
        // Select tree based on previous character (1st-order conditional)
        // A/65 Annex C: Context is the previous 7-bit character
        int context = (prev_char & 0x7F);
        HuffmanNode *tree = base_trees + (context * nodes_per_tree);
        int node_idx = 0;

        // Traverse tree
        while (node_idx >= 0) {
            if (bit_pos >= max_bits) break;
            int bit = get_bit(src, bit_pos++);
            node_idx = tree[node_idx].children[bit];
        }

        if (node_idx < 0) {
            unsigned char c = (unsigned char)(-(node_idx + 1));
            if (c == 0x00) break; // End of string
            dest[dest_pos++] = c;
            prev_char = c;
        } else {
            // Incomplete traversal
            break;
        }
    }

    dest[dest_pos] = '\0';
    return 1;
}
