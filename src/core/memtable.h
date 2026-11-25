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

typedef struct FDBTree {
    FDBNode *root;
    size_t count;
    size_t bytes;
    size_t key_bytes;
} FDBTree;

bool fdb_node_size(FastDBSlice key, FastDBSlice value, size_t *size);
FDBNode *fdb_node_create(const FDBRecord *record, size_t bytes);
const FDBNode *fdb_tree_get(const FDBTree *tree, FastDBSlice key);
const FDBNode *fdb_tree_bound(const FDBTree *tree, FastDBSlice key, bool exclusive);
void fdb_tree_put(FDBTree *tree, FDBNode *node);
void fdb_tree_delete(FDBTree *tree, FastDBSlice key);
void fdb_tree_destroy(FDBTree *tree);

#endif
