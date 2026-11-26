#include "memtable.h"

#include <stdlib.h>
#include <string.h>

bool fdb_node_size(FastDBSlice key, FastDBSlice value, size_t *size)
{
    return fdb_add_size(sizeof(FDBNode), key.size, size) &&
           fdb_add_size(*size, value.size, size);
}

static size_t node_size(const FDBNode *node)
{
    return sizeof(*node) + node->record.key.size + node->record.value.size;
}

FDBNode *fdb_node_create(const FDBRecord *record, size_t bytes)
{
    FDBNode *node = malloc(bytes);
    if (node == NULL)
        return NULL;
    *node = (FDBNode){.record = *record, .red = true};
    memcpy(node->bytes, record->key.data, record->key.size);
    if (record->value.size != 0)
        memcpy(node->bytes + record->key.size, record->value.data, record->value.size);
    node->record.key.data = node->bytes;
    node->record.value.data = node->bytes + record->key.size;
    return node;
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
        tree->key_bytes += node->record.key.size;
    } else {
        tree->bytes -= node_size(replaced);
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

static FDBNode *remove_key(FDBNode *node, FastDBSlice key)
{
    if (fdb_compare(key, node->record.key) < 0) {
        if (!is_red(node->left) && !is_red(node->left->left))
            node = move_red_left(node);
        node->left = remove_key(node->left, key);
    } else {
        if (is_red(node->left))
            node = rotate_right(node);
        if (fdb_compare(key, node->record.key) == 0 && node->right == NULL) {
            free(node);
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
            free(node);
            node = successor;
        } else {
            node->right = remove_key(node->right, key);
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
    tree->key_bytes -= node->record.key.size;
    --tree->count;
    if (!is_red(tree->root->left) && !is_red(tree->root->right))
        tree->root->red = true;
    tree->root = remove_key(tree->root, key);
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
    destroy(tree->root);
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
    out->key_bytes = source->key_bytes;
    return FASTDB_OK;
}
