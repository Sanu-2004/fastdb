#ifndef FASTDB_MEMTABLE_H
#define FASTDB_MEMTABLE_H

#include "record.h"

typedef struct FDBNode {
    struct FDBNode *left;
    struct FDBNode *right;
    FDBRecord record;
    bool red;
    unsigned char bytes[];
} FDBNode;

typedef struct FDBArenaChunk FDBArenaChunk;

typedef struct FDBTree {
    FDBNode *root;
    size_t count;
    size_t bytes;
    FDBArenaChunk *arena;
} FDBTree;

typedef struct FDBArenaWrite {
    FDBArenaChunk *chunk;
    size_t offset;
} FDBArenaWrite;

bool fdb_node_size(FastDBSlice key, FastDBSlice value, size_t *size);
FDBNode *fdb_node_create(const FDBRecord *record, size_t bytes);
bool fdb_arena_chunk_size(size_t capacity, size_t *size);
bool fdb_arena_count_node(size_t bytes, size_t *payload);
/* One reservation per staged commit. A failed commit must discard the reservation. */
FastDBStatus fdb_tree_reserve_arena(const FDBTree *tree, size_t payload, size_t chunk_bytes,
                                    size_t spare, FDBArenaWrite *write);
FDBNode *fdb_arena_make_node(FDBArenaWrite *write, const FDBRecord *record, size_t bytes);
void fdb_tree_finish_arena(FDBTree *tree, FDBArenaWrite write, bool commit);
size_t fdb_tree_memory_bytes(const FDBTree *tree);
const FDBNode *fdb_tree_get(const FDBTree *tree, FastDBSlice key);
const FDBNode *fdb_tree_bound(const FDBTree *tree, FastDBSlice key, bool exclusive);
void fdb_tree_put(FDBTree *tree, FDBNode *node);
void fdb_tree_delete(FDBTree *tree, FastDBSlice key);
void fdb_tree_destroy(FDBTree *tree);
FastDBStatus fdb_tree_clone(const FDBTree *source, FDBTree *out);

#endif
