#include "../libs/markdown.h"
#include <stdlib.h>
#include <string.h>

#define SUCCESS 0 
#define INVALID_POS -1
#define VERSION_ERROR -2
#define MODIFIED 1
#define NOT_MODIFIED 0
#define NORMAL_TEXT 0
#define ORDERED_LIST -1
#define UNORDERED_LIST -2
#define NEWLINE -3
#define True 1
#define False 0

#define INVALID_CURSOR_POS -1
#define DELETED_POSITION -2
#define OUTDATED_VERSION -3

// === My own function ===
chunk* find_chunk_at(document *doc, size_t global_pos, size_t *local_pos) {
    // cur refers to current chunk
    chunk *cur = doc->head;
    size_t pos = 0; // accumulating amount in total

    while (cur != NULL) {
        // skip the deleted chunk

        // detect if this chunk cover the traget
        if (pos + cur->length > global_pos) {
            *local_pos = global_pos - pos; // store the value
            return cur;
        }
        pos += cur->length;
        cur = cur->next;
    }
    
    // means it is at the end of the document
    return NULL;
}

chunk* create_chunk(const char *text, size_t len){
    chunk *new_chunk = malloc(sizeof(chunk));
    new_chunk->next = NULL;
    
    // malloc the text
    new_chunk->text = malloc(sizeof(char) * len + 1);
    if (new_chunk->text == NULL) {
        free(new_chunk);
        return NULL;
    }

    // copy the memory, copy the length
    memcpy(new_chunk->text, text, len);
    new_chunk->text[len] = '\0';
    new_chunk->length = len;

    // initialize the type
    new_chunk->type = NORMAL_TEXT;
    new_chunk->ready_to_delete = False;

    // return the chunk
    return new_chunk;
}

chunk* split_chunk(chunk *c, size_t pos) {
    if (pos > c->length) return NULL;

    if (pos >= c->length) return c->next;

    // create a new chunk
    chunk *new_chunk = malloc(sizeof(chunk));
    if (!new_chunk) return NULL;

    // malloc the right memory sapce
    size_t new_len = c->length - pos;
    new_chunk->text = malloc(new_len + 1);  // for '\0'

    // copy the right part
    memcpy(new_chunk->text, c->text + pos, new_len);
    new_chunk->text[new_len] = '\0';
    new_chunk->length = new_len;
    
    if (c->type == NEWLINE) {
        new_chunk->type = NEWLINE;
    }else {
        new_chunk->type = NORMAL_TEXT;
    }

    if (c->ready_to_delete == True){
        new_chunk->ready_to_delete = True;
    }
    
    // realloc the left part. pos is the length of left part
    c->text = realloc(c->text, pos + 1);

    // set the left chunk
    c->text[pos] = '\0';
    c->length = pos;
    if (c->length == 0) {
        c->type = NORMAL_TEXT;
    }

    // maintain the list order
    new_chunk->next = c->next;
    c->next = new_chunk;

    return new_chunk;
}

/**
 * This function is used to determine whether this pos start a newline
 */
int is_newline_before(document* doc, size_t pos) {
    if (!doc) return False;
    
    // head of the document means a newline
    if (pos == 0) return True;

    // find chunk at pos - 1
    size_t local_pos = 0;
    chunk* target = find_chunk_at(doc, pos - 1, &local_pos);
    if (!target) {
        // at the end
        return False;
    }
    
    if (target->type == NEWLINE){
        return True;
    } else{
        return False;
    }
}

// === Init and Free ===
document *markdown_init(void) {
    // malloc the memory for doc
    document *doc = malloc(sizeof(document));
    if (!doc) return NULL;

    doc->head = NULL;
    doc->version = 0;
    doc->is_modify = NOT_MODIFIED;
    
    // init a dynamic empty string
    doc->current_version = malloc(1);
    if (doc->current_version) {
        doc->current_version[0] = '\0';
    }

    return doc;
}

void markdown_free(document *doc) {
    if (!doc) return;

    // free the current_text
    free(doc->current_version);
    
    // free each chunk
    chunk *curr = doc->head;
    while (curr) {
        chunk *next = curr->next; // record the next chunk
        free(curr->text);
        free(curr);
        curr = next;
    }

    free(doc); // free the doc itself
}

// === Edit Commands ===
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content) {
    if (!doc || !content) return INVALID_POS;

    // use my function to find chunk and its local position
    size_t local_pos = 0;
    chunk *target = find_chunk_at(doc, pos, &local_pos);

    // create a new chunk
    chunk *new_chunk = create_chunk(content, strlen(content));

    // if file is empty
    if (doc->head == NULL) {
        doc->head = new_chunk;
        update_modification(doc, version);
        return SUCCESS;
    }
    
    // if pos is at the end of the document
    if (target == NULL) {
        chunk *cur = doc->head;
        while (cur->next) cur = cur->next;
        cur->next = new_chunk;
        update_modification(doc, version);
        return SUCCESS;
    }
    
    // find the right chunk, but it could be NULL
    chunk* right = split_chunk(target, local_pos);
    if (right == NULL){
        target->next = new_chunk;
        update_modification(doc, version);
        return SUCCESS;
    }

    // normal case: maintain the order
    new_chunk->next = right;
    target->next = new_chunk;

    update_modification(doc, version);
    return SUCCESS;
}

int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len) {
    if (doc == NULL || doc->head == NULL || len == 0) return SUCCESS;

    // split at the starting point
    size_t start_pos = 0;
    chunk* start_target = find_chunk_at(doc, pos, &start_pos);
    if (start_target == NULL) return SUCCESS; // we don't need to delete anything
    split_chunk(start_target, start_pos);
    
    // split at the end point
    size_t end_pos = 0;
    chunk* end_target = find_chunk_at(doc, pos + len, &end_pos);
    chunk *iter_end = NULL;

    if (end_target != NULL) {
        iter_end = split_chunk(end_target, end_pos);
    }

    // delete all the middle part
    chunk* iter_cur = start_target;
    while (iter_cur->next != iter_end){
        iter_cur->next->ready_to_delete = True;
        iter_cur = iter_cur->next;
    }

    update_modification(doc, version);
    return SUCCESS;
}

// === Formatting Commands ===
int markdown_newline(document *doc, uint64_t version, int pos) {
    // find insert position and break any list
    size_t local_position = 0;
    chunk* target = find_chunk_at(doc, pos, &local_position);
    if (target && target->type != NEWLINE){
        target->type = NORMAL_TEXT;
    }

    // init a newline chunk
    chunk* newline_chunk = create_chunk("\n", strlen("\n"));
    newline_chunk->type = NEWLINE;
    
    // means at the end of the document
    if (!target){
        chunk *cur = doc->head;
        while (cur->next) cur = cur->next;
        cur->next = newline_chunk;
        update_modification(doc, version);
        return SUCCESS;
    }

    chunk* right = split_chunk(target, local_position); // split the original list
    
    // maintain the order
    newline_chunk->next = right;
    target->next = newline_chunk;

    maintain_list_order(doc->head); // maintain the whole list order

    update_modification(doc, version);
    return SUCCESS;
}

int markdown_heading(document *doc, uint64_t version, int level, size_t pos) {
    if (level < 1 || level > 3) return INVALID_POS;
    
    // create a temp char to bulid the string firstly
    char temp[10];
    memset(temp, 0, sizeof(temp));
    for (int i = 0; i < level; i++) {
        temp[i] = '#';
    }
    temp[level] = ' '; // required in the pfd document
    temp[level + 1] = '\0'; // make it a string since we use strlen
    
    // we haven't making sure there is a newline when we call a heading
    markdown_insert(doc, version, pos, temp);

    // This is a Block-level Element
    if (is_newline_before(doc, pos) != True){
        markdown_newline(doc, version, pos);
    }
    
    update_modification(doc, version);
    return SUCCESS;
}

int markdown_bold(document *doc, uint64_t version, size_t start, size_t end) {
    if (start > end) return INVALID_POS;

    // insert the end firstly avoiding modifying start pos again
    markdown_insert(doc, version, end, "**");
    markdown_insert(doc, version, start, "**");

    update_modification(doc, version);
    return SUCCESS;
}

int markdown_italic(document *doc, uint64_t version, size_t start, size_t end) {
    if (start > end) return INVALID_POS;

    // same as the pervious one
    markdown_insert(doc, version, end, "*");
    markdown_insert(doc, version, start, "*");

    update_modification(doc, version);
    return SUCCESS;
}

int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    markdown_insert(doc, version, pos, "> ");

    // This is a Block-level Element
    if (is_newline_before(doc, pos) != True){
        markdown_newline(doc, version, pos);
    }
    
    update_modification(doc, version);
    return SUCCESS;
}

int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    if (!doc) return INVALID_POS;

    // find target firstly
    size_t local_pos = 0;
    chunk* target = find_chunk_at(doc, pos, &local_pos);
    if (!target) return INVALID_POS;

    // insert the list number chunk
    char temp[10];
    strcpy(temp, "1. ");
    chunk* new_chunk = create_chunk(temp, strlen(temp));
    new_chunk->type = ORDERED_LIST; // we have to reassign order in following function
    chunk* right = split_chunk(target, local_pos);
    target->next = new_chunk;
    new_chunk->next = right;

    // This is a Block-level Element
    if (is_newline_before(doc, pos) != True){
        markdown_newline(doc, version, pos);
    }

    maintain_list_order(doc->head);

    update_modification(doc, version);
    return SUCCESS;
}


/**
 * Start: from the start chunk, maintain the list order, rules as below:
 * whenever we find a newline chunk, if next one is list unmber chunk trace and update the unmber. 
 * If we find a normal text, reset the unmber.
 */
int maintain_list_order(chunk* start){
    if (start == NULL) return SUCCESS;

    int cur_order;

    // special case: start is a ordered list
    chunk* cur_c = start;
    
    // skip all empty
    while (cur_c)
    {
        if (cur_c->length == 0){
            cur_c = cur_c->next;
        } else {
            break;
        }
    }

    if (cur_c->type == ORDERED_LIST){
        cur_order = 2;
    } else {
        cur_order = 1;
    }
    
    while (cur_c) {
        // determine the newline
        if (cur_c->type != NEWLINE) {
            cur_c = cur_c->next;
            continue;
        }

        // skip all empty
        chunk* n = cur_c->next;
        while (n && n->length == 0) {
            n = n->next;
        }

        if (!n) {
            break;
        }

        if (n->type != ORDERED_LIST) {
            cur_order = 1;
            cur_c = n;
            continue;
        }

        // update the unmber
        n->text[0] = '0' + cur_order;
        cur_order++;

        cur_c = n;
    }

    return SUCCESS;
}


int markdown_unordered_list(document *doc, uint64_t version, size_t pos){
    if (!doc) return INVALID_POS;

    size_t local_pos = 0;
    chunk *target = find_chunk_at(doc, pos, &local_pos);

    // only transfer normal text
    if (!target || target->type != NORMAL_TEXT) {
        return INVALID_POS;
    }

    // insert a "- "
    chunk* new_chunk = create_chunk("- ", strlen("- "));
    new_chunk->type = UNORDERED_LIST;
    chunk* right = split_chunk(target, local_pos);
    target->next = new_chunk;
    new_chunk->next = right;

    // This is a Block-level Element
    if (is_newline_before(doc, pos) != True){
        markdown_newline(doc, version, pos);
    }

    update_modification(doc, version);
    return SUCCESS;
}

int markdown_code(document *doc, uint64_t version, size_t start, size_t end){
    if (!doc) return INVALID_POS;

    // insert the "`"
    markdown_insert(doc, version, end, "`");
    markdown_insert(doc, version, start, "`");

    update_modification(doc, version);
    return SUCCESS;
}

int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos){
    if (!doc) return INVALID_POS;

    // Insert "---\n"
    markdown_newline(doc, version, pos);
    markdown_insert(doc, version, pos, "---");

    // This is a Block-level Element
    if (is_newline_before(doc, pos) != True){
        markdown_newline(doc, version, pos);
    }

    update_modification(doc, version);
    return SUCCESS;
}

int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url) {
    if (!doc) return INVALID_POS;

    // insert
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "](%s)", url);
    markdown_insert(doc, version, end, buffer);
    markdown_insert(doc, version, start, "[");

    update_modification(doc, version);
    return SUCCESS;
}

// === Utilities ===
void markdown_print(const document *doc, FILE *stream) {
    if (!doc || !stream) return;
    
    // iterate all chunk and write data
    chunk *cur = doc->head;
    while (cur) {
        fwrite(cur->text, sizeof(char), cur->length, stream);
        cur = cur->next;
    }
}

char *markdown_flatten(const document *doc) {
    if (!doc || !doc->current_version) return NULL;

    size_t len = strlen(doc->current_version);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    
    strcpy(copy, doc->current_version);
    return copy;
}

void markdown_update_current_version(document* doc){
    if (!doc || !doc->head) return;

    // calculate the total len
    size_t total_len = 0;
    chunk *cur = doc->head;
    while (cur) {
        total_len += cur->length;
        cur = cur->next;
    }

    // malloc memory. but I am not sure if the test function is going to free it?
    char *result = malloc(total_len + 1);
    if (!result) return;

    // write all chunk into malloc memory
    size_t pos = 0;
    cur = doc->head;
    while (cur) {
        memcpy(result + pos, cur->text, cur->length);
        pos += cur->length;
        cur = cur->next;
    }

    // add a end mark to make it a string
    result[total_len] = '\0';

    doc->current_version = result;
}

// === Versioning ===
void markdown_increment_version(document *doc) {
    if (doc->is_modify == NOT_MODIFIED) return;

    chunk *prev = NULL;
    chunk *cur  = doc->head;

    while (cur) {
        chunk *next = cur->next; // record next firstly

        // remove all deleted chunk and 0 chunk
        if (cur->ready_to_delete == True || cur->length == 0) {
            if (prev) {
                prev->next = next;
            } else {
                doc->head = next;
            }
            
            // free the chunk
            free(cur->text);
            free(cur);
        } else {
            // update prev
            prev = cur;
        }
        cur = next;
    }

    // update the version char
    free(doc->current_version);
    markdown_update_current_version(doc);

    // increment verison and reset the MODIFIED
    doc->version++;
    doc->is_modify = NOT_MODIFIED;
}

/**
 * This function is used to detect if the version is up to date. Since in the requirement, it states
 * Any command referencing an outdated version will be rejected.
 */
int validate_version(document *doc, uint64_t version) {
    if (!doc) return INVALID_POS;
    if (version != doc->version) return VERSION_ERROR;
    return SUCCESS;
}

/**
 * This function is used to update the is_modified variable after every modification
 */
void update_modification(document* doc, uint64_t version) {
    doc->is_modify = MODIFIED;
}