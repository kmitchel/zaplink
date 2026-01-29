/**
 * @file huffman.h
 * @brief ATSC A/65 Huffman text decoder
 * 
 * Decodes Huffman-compressed text from ATSC PSIP tables.
 * ATSC uses two compression schemes defined in A/65 Annex C:
 * - Type 0x01: Program title compression
 * - Type 0x02: Program description compression
 * 
 * Both use 1st-order conditional Huffman coding with 128 trees
 * (one per preceding character). The huffman.bin file contains
 * pre-built decoding tables from the A/65 specification.
 */

#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stddef.h>
#include <stdint.h>

/**
 * Huffman tree node structure
 * Each node is 4 bytes containing two 16-bit child indices.
 * Leaf nodes are encoded as negative values: value = -(index + 1)
 */
typedef struct {
    int16_t children[2];
} HuffmanNode;

/**
 * Huffman decoding table for 1st-order conditional coding
 * Contains 128 trees (one per preceding ASCII character)
 */
typedef struct {
    HuffmanNode *trees;     /**< Array of tree nodes */
    int nodes_per_tree;     /**< Fixed size of each tree */
} HuffmanTable;

/**
 * Initialize the Huffman decoder
 * Loads huffman.bin from the current working directory
 * @return 1 on success, 0 on failure (file not found, parse error)
 */
int huffman_init();

/**
 * Clean up Huffman decoder resources
 */
void huffman_cleanup();

/**
 * Decode an ATSC Huffman-compressed text segment
 * 
 * @param compr_type Compression type (0x01 = title, 0x02 = description)
 * @param src        Compressed bitstream
 * @param src_len    Length of compressed data in bytes
 * @param dest       Output buffer for decoded string
 * @param dest_len   Size of output buffer
 * @return 1 on success, 0 on decoding failure
 */
int huffman_decode(int compr_type, const uint8_t *src, int src_len, 
                   char *dest, int dest_len);

#endif
