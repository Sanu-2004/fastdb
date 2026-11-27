#include "memtable.h"

#include <stdlib.h>
#include <string.h>

struct FDBArenaChunk {
    FDBArenaChunk *previous;
    size_t capacity;
    size_t used;
    size_t total_bytes;
    _Alignas(FDBNode) unsigned char data[];
};

bool fdb_node_size(FastDBSlice key, FastDBSlice value, size_t *size)
{
    return fdb_add_size(sizeof(FDBNode), key.size, size) &&
           fdb_add_size(*size, value.size, size);
}

static size_t node_size(const FDBNode *node)
{
    return sizeof(*node) + node->record.key.size + node->record.value.size;
}

static FDBNode *initialize_node(void *memory, const FDBRecord *record)
{
    FDBNode *node = memory;
    *node = (FDBNode){.record = *record, .red = true};
    memcpy(node->bytes, record->key.data, record->key.size);
    if (record->value.size != 0)
        memcpy(node->bytes + record->key.size, record->value.data, record->value.size);
    node->record.key.data = node->bytes;
    node->record.value.data = node->bytes + record->key.size;
    return node;
}

FDBNode *fdb_node_create(const FDBRecord *record, size_t bytes)
{
    void *memory = malloc(bytes);
    return memory == NULL ? NULL : initialize_node(memory, record);
}

bool fdb_arena_chunk_size(size_t capacity, size_t *size)
{
    return fdb_add_size(sizeof(FDBArenaChunk), capacity, size);
}

static size_t alignment_padding(size_t bytes)
{
    const size_t alignment = _Alignof(FDBNode);
    return (alignment - bytes % alignment) % alignment;
}

bool fdb_arena_count_node(size_t bytes, size_t *payload)
{
    return fdb_add_size(bytes, alignment_padding(bytes), &bytes) &&
           fdb_add_size(*payload, bytes, payload);
}

FastDBStatus fdb_tree_reserve_arena(const FDBTree *tree, size_t payload, size_t chunk_bytes,
                                    size_t spare, FDBArenaWrite *write)
{
    FDBArenaChunk *current = tree->arena;
    if (current != NULL && payload <= current->capacity - current->used) {
        *write = (FDBArenaWrite){current, current->used};
        return FASTDB_OK;
    }

    size_t capacity = payload > chunk_bytes ? payload : chunk_bytes;
    size_t bytes, total;
    size_t retained = current == NULL ? 0 : current->total_bytes;
    if (!fdb_arena_chunk_size(capacity, &bytes) || bytes > spare ||
        !fdb_add_size(retained, bytes, &total))
        return FASTDB_LIMIT_EXCEEDED;
    FDBArenaChunk *chunk = malloc(bytes);
    if (chunk == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *chunk = (FDBArenaChunk){.previous = current, .capacity = capacity, .total_bytes = total};
    *write = (FDBArenaWrite){.chunk = chunk};
    return FASTDB_OK;
}

FDBNode *fdb_arena_make_node(FDBArenaWrite *write, const FDBRecord *record, size_t bytes)
{
    void *memory = write->chunk->data + write->offset;
    /* The reservation has already checked and included padding for this node. */
    write->offset += bytes + alignment_padding(bytes);
    return initialize_node(memory, record);
}

void fdb_tree_finish_arena(FDBTree *tree, FDBArenaWrite write, bool commit)
{
    if (write.chunk == NULL)
        return;
    if (commit) {
        write.chunk->used = write.offset;
        tree->arena = write.chunk;
    } else if (write.chunk != tree->arena) {
        free(write.chunk);
    }
}

size_t fdb_tree_memory_bytes(const FDBTree *tree)
{
    return tree->arena == NULL ? tree->bytes : tree->arena->total_bytes;
}

const FDBNode *fdb_tree_get(const FDBTree *tree, FastDBSlice key)
{
    const FDBNode *node = tree->root;
    while (node != NULL) {
        int order = fdb_compare(key, node->record.key);
        if (order == 0)
            return node;
        node = order < 0 ? node->left : node->right;
    }
    return NULL;
}

const FDBNode *fdb_tree_bound(const FDBTree *tree, FastDBSlice key, bool exclusive)
{
    const FDBNode *bound = NULL;
    const FDBNode *node = tree->root;
    while (node != NULL) {
        int order = fdb_compare(key, node->record.key);
        if (order < 0 || (order == 0 && !exclusive)) {
            bound = node;
            node = node->left;
        } else {
            node = node->right;
        }
    }
    return bound;
}

static bool is_red(const FDBNode *node)
{
    return node != NULL && node->red;
}

static FDBNode *rotate_left(FDBNode *node)
{
    FDBNode *right = node->right;
    node->right = right->left;
    right->left = node;
    right->red = node->red;
    node->red = true;
    return right;
}

static FDBNode *rotate_right(FDBNode *node)
{
    FDBNode *left = node->left;
    node->left = left->right;
    left->right = node;
    left->red = node->red;
    node->red = true;
    return left;
}

static void flip_colors(FDBNode *node)
{
    node->red = !node->red;
    node->left->red = !node->left->red;
    node->right->red = !node->right->red;
}

static FDBNode *balance(FDBNode *node)
{
    if (is_red(node->right) && !is_red(node->left))
        node = rotate_left(node);
    if (is_red(node->left) && is_red(node->left->left))
        node = rotate_right(node);
    if (is_red(node->left) && is_red(node->right))
        flip_colors(node);
    return node;
}

static FDBNode *insert(FDBNode *root, FDBNode *node, FDBNode **replaced)
{
    if (root == NULL)
        return node;
    int order = fdb_compare(node->record.key, root->record.key);
    if (order < 0)
        root->left = insert(root->left, node, replaced);
    else if (order > 0)
        root->right = insert(root->right, node, replaced);
    else {
        node->left = root->left;
        node->right = root->right;
        node->red = root->red;
        *replaced = root;
        return node;
    }
    return balance(root);
}

void fdb_tree_put(FDBTree *tree, FDBNode *node)
{
    FDBNode *replaced = NULL;
    tree->root = insert(tree->root, node, &replaced);
    tree->root->red = false;
    if (replaced == NULL) {
        ++tree->count;
    } else {
        tree->bytes -= node_size(replaced);
        if (tree->arena == NULL)
            free(replaced);
    }
    tree->bytes += node_size(node);
}

/* Transfer a red link before descending into a black 2-node. */
static FDBNode *move_red_left(FDBNode *node)
{
    flip_colors(node);
    if (is_red(node->right->left)) {
        node->right = rotate_right(node->right);
        node = rotate_left(node);
        flip_colors(node);
    }
    return node;
}

static FDBNode *move_red_right(FDBNode *node)
{
    flip_colors(node);
    if (is_red(node->left->left)) {
        node = rotate_right(node);
        flip_colors(node);
    }
    return node;
}

static FDBNode *remove_minimum(FDBNode *node, FDBNode **minimum)
{
    if (node->left == NULL) {
        *minimum = node;
        return node->right;
    }
    if (!is_red(node->left) && !is_red(node->left->left))
        node = move_red_left(node);
    node->left = remove_minimum(node->left, minimum);
    return balance(node);
}

static FDBNode *remove_key(FDBNode *node, FastDBSlice key, FDBNode **removed)
{
    if (fdb_compare(key, node->record.key) < 0) {
        if (!is_red(node->left) && !is_red(node->left->left))
            node = move_red_left(node);
        node->left = remove_key(node->left, key, removed);
    } else {
        if (is_red(node->left))
            node = rotate_right(node);
        if (fdb_compare(key, node->record.key) == 0 && node->right == NULL) {
            *removed = node;
            return NULL;
        }
        if (!is_red(node->right) && !is_red(node->right->left))
            node = move_red_right(node);
        if (fdb_compare(key, node->record.key) == 0) {
            FDBNode *successor;
            FDBNode *right = remove_minimum(node->right, &successor);
            successor->left = node->left;
            successor->right = right;
            successor->red = node->red;
            *removed = node;
            node = successor;
        } else {
            node->right = remove_key(node->right, key, removed);
        }
    }
    return balance(node);
}

void fdb_tree_delete(FDBTree *tree, FastDBSlice key)
{
    const FDBNode *node = fdb_tree_get(tree, key);
    if (node == NULL)
        return;
    tree->bytes -= node_size(node);
    --tree->count;
    if (!is_red(tree->root->left) && !is_red(tree->root->right))
        tree->root->red = true;
    FDBNode *removed = NULL;
    tree->root = remove_key(tree->root, key, &removed);
    if (tree->arena == NULL)
        free(removed);
    if (tree->root != NULL)
        tree->root->red = false;
}

static void destroy(FDBNode *node)
{
    if (node != NULL) {
        destroy(node->left);
        destroy(node->right);
        free(node);
    }
}

void fdb_tree_destroy(FDBTree *tree)
{
    if (tree->arena == NULL) {
        destroy(tree->root);
    } else {
        FDBArenaChunk *chunk = tree->arena;
        while (chunk != NULL) {
            FDBArenaChunk *previous = chunk->previous;
            free(chunk);
            chunk = previous;
        }
    }
    *tree = (FDBTree){0};
}

static FastDBStatus clone_node(const FDBNode *source, FDBNode **out)
{
    if (source == NULL)
        return FASTDB_OK;
    FDBNode *node = fdb_node_create(&source->record, node_size(source));
    if (node == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *out = node;
    node->red = source->red;
    FastDBStatus status = clone_node(source->left, &node->left);
    if (status == FASTDB_OK)
        status = clone_node(source->right, &node->right);
    return status;
}

FastDBStatus fdb_tree_clone(const FDBTree *source, FDBTree *out)
{
    *out = (FDBTree){0};
    FastDBStatus status = clone_node(source->root, &out->root);
    if (status != FASTDB_OK) {
        fdb_tree_destroy(out);
        return status;
    }
    out->count = source->count;
    out->bytes = source->bytes;
    return FASTDB_OK;
}
