#ifndef DOCUMENT_H

#define DOCUMENT_H
/**
 * This file is the header file for all the document functions. You will be tested on the functions inside markdown.h
 * You are allowed to and encouraged multiple helper functions and data structures, and make your code as modular as possible. 
 * Ensure you DO NOT change the name of document struct.
 */


/**
 * each chunk just refer to a block of text. 
 * There is no limitation for each chunk. It will lose performance but good to implement.
 * For the order part, -1 indicates normal text, 0 indicates unordered list, 1-9 indicates ordered list and its order.
 */
typedef struct chunk {
    size_t length;
    struct chunk* next;
    char* text;
    int type;
    int ready_to_delete;
} chunk;

typedef struct {
    // TODO
    char *current_version; // the current printed version. not need to modify
    chunk *head; // pointing to the first chunk
    int is_modify; // use to determine wheater this document is modified the period of time
    uint64_t version; // version number
} document;

// Functions from here onwards.
/**
 * Given a pos indicate the character position in a golbal text view. For example in "abcd", d is at the fourth position
 * We find which chunk is the character located in and return this chunk
 * Besides, stroe its position in this chunk to *local_pos.
 */
chunk* find_chunk_at(document *doc, size_t global_pos, size_t *local_pos);
/**
 * create a new chunck with the given text
 */
chunk* create_chunk(const char *text, size_t len);
/**
 * spilit a chunk form the given position, the linked relationship is stay
 */
chunk* split_chunk(chunk *c, size_t pos);
#endif
