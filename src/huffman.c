#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdint.h>
#include <limits.h>
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

static int read_exact(int fd, void *buffer, size_t length) {
    unsigned char *position = buffer;
    while (length > 0) {
        ssize_t count = read(fd, position, length);
        if (count <= 0) return 0;
        position += count;
        length -= (size_t)count;
    }
    return 1;
}

static int table_has_codes(const HuffmanNode *trees, int tree_nodes) {
    int useful_root = 0;
    int nonempty_leaf = 0;
    for (int context = 0; context < 128; context++) {
        const HuffmanNode *tree = trees + (size_t)context * tree_nodes;
        if (tree[0].children[0] != -1 || tree[0].children[1] != -1) {
            useful_root = 1;
        }
        for (int node = 0; node < tree_nodes; node++) {
            for (int bit = 0; bit < 2; bit++) {
                int child = tree[node].children[bit];
                if (child < -1) nonempty_leaf = 1;
                if (child >= tree_nodes) return 0;
            }
        }
    }
    return useful_root && nonempty_leaf;
}

static int huffman_load(void) {
    int fd = open("huffman.bin", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        // Not a fatal error, but decoding won't work
        LOG_DEBUG("HUFFMAN", "huffman.bin not found, Huffman decoding disabled");
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(HuffmanHeader)) {
        close(fd);
        return 0;
    }

    HuffmanHeader header;
    if (!read_exact(fd, &header, sizeof(header))) {
        close(fd);
        return 0;
    }

    if (memcmp(header.magic, HUFFMAN_MAGIC, 4) != 0 || header.version != HUFFMAN_VERSION) {
        LOG_WARN("HUFFMAN", "Invalid huffman.bin format or version");
        close(fd);
        return 0;
    }

    if (header.nodes_per_tree == 0 || header.nodes_per_tree > INT_MAX) {
        LOG_WARN("HUFFMAN", "Invalid Huffman tree size");
        close(fd);
        return 0;
    }
    size_t tree_size = 128U * (size_t)header.nodes_per_tree * sizeof(HuffmanNode);
    if ((uint64_t)header.title_offset + tree_size > (uint64_t)st.st_size ||
        (uint64_t)header.desc_offset + tree_size > (uint64_t)st.st_size) {
        LOG_WARN("HUFFMAN", "Huffman table offsets exceed file size");
        close(fd);
        return 0;
    }
    nodes_per_tree = (int)header.nodes_per_tree;

    title_trees = malloc(tree_size);
    desc_trees = malloc(tree_size);

    if (!title_trees || !desc_trees) {
        perror("malloc");
        free(title_trees);
        free(desc_trees);
        title_trees = NULL;
        desc_trees = NULL;
        close(fd);
        return 0;
    }

    int loaded = lseek(fd, (off_t)header.title_offset, SEEK_SET) >= 0 &&
                 read_exact(fd, title_trees, tree_size) &&
                 lseek(fd, (off_t)header.desc_offset, SEEK_SET) >= 0 &&
                 read_exact(fd, desc_trees, tree_size);

    close(fd);
    if (!loaded) {
        LOG_WARN("HUFFMAN", "Unable to read complete Huffman tables");
        free(title_trees);
        free(desc_trees);
        title_trees = NULL;
        desc_trees = NULL;
        nodes_per_tree = 0;
        return 0;
    }
    if (!table_has_codes(title_trees, nodes_per_tree) ||
        !table_has_codes(desc_trees, nodes_per_tree)) {
        LOG_ERROR("HUFFMAN", "huffman.bin contains placeholder or invalid decode tables");
        free(title_trees);
        free(desc_trees);
        title_trees = desc_trees = NULL;
        nodes_per_tree = 0;
        return 0;
    }
    LOG_DEBUG("HUFFMAN", "Tables loaded (%d nodes per tree)", nodes_per_tree);
    return 1;
}

int huffman_init(void) {
    pthread_mutex_lock(&huffman_mutex);
    if (!huffman_initialized) {
        int loaded = huffman_load();
        huffman_initialized = 1;
        pthread_mutex_unlock(&huffman_mutex);
        return loaded;
    }
    int loaded = title_trees != NULL && desc_trees != NULL;
    pthread_mutex_unlock(&huffman_mutex);
    return loaded;
}

void huffman_cleanup() {
    pthread_mutex_lock(&huffman_mutex);
    if (title_trees) free(title_trees);
    if (desc_trees) free(desc_trees);
    title_trees = NULL;
    desc_trees = NULL;
    nodes_per_tree = 0;
    huffman_initialized = 0;
    pthread_mutex_unlock(&huffman_mutex);
}

static int get_bit(const uint8_t *src, int bit_pos) {
    return (src[bit_pos / 8] >> (7 - (bit_pos % 8))) & 1;
}

int huffman_decode(int compr_type, const uint8_t *src, int src_len, char *dest, int dest_len) {
    if ((compr_type != 1 && compr_type != 2) || !src || src_len <= 0 ||
        !dest || dest_len <= 0 || src_len > INT_MAX / 8) return 0;
    pthread_mutex_lock(&huffman_mutex);
    if (!huffman_initialized) {
        huffman_load();
        huffman_initialized = 1;
    }
    pthread_mutex_unlock(&huffman_mutex);

    HuffmanNode *base_trees = (compr_type == 1) ? title_trees : desc_trees;
    if (!base_trees) return 0;

    int bit_pos = 0;
    int max_bits = src_len * 8;
    size_t existing = strnlen(dest, (size_t)dest_len);
    if (existing >= (size_t)dest_len) return 0;
    int dest_pos = (int)existing;
    int prev_char = 0; // Standard says initial context is 0x00

    for (;;) {
        if (bit_pos >= max_bits) break; // No end-of-string code
        unsigned char c;
        if (prev_char >= 128) {
            // A/65 Annex C: characters after Latin-1 bytes are uncompressed.
            if (max_bits - bit_pos < 8) break;
            c = 0;
            for (int i = 0; i < 8; i++) c = (unsigned char)((c << 1) | get_bit(src, bit_pos++));
        } else {
            HuffmanNode *tree = base_trees + (prev_char * nodes_per_tree);
            int node_idx = 0;
            while (node_idx >= 0 && bit_pos < max_bits) {
                if (node_idx >= nodes_per_tree) break;
                node_idx = tree[node_idx].children[get_bit(src, bit_pos++)];
            }
            if (node_idx >= 0) break; // Truncated code or invalid tree
            c = (unsigned char)(-(node_idx + 1));
            if (c == 27) {
                // The symbol after an order-1 escape is a literal 8-bit byte.
                if (max_bits - bit_pos < 8) break;
                c = 0;
                for (int i = 0; i < 8; i++) c = (unsigned char)((c << 1) | get_bit(src, bit_pos++));
            }
        }
        if (c == 0) {
            dest[dest_pos] = '\0';
            return 1;
        }
        if (dest_pos >= dest_len - 1) break;
        dest[dest_pos++] = (char)c;
        prev_char = c;
    }

    dest[dest_pos] = '\0';
    return 0;
}
