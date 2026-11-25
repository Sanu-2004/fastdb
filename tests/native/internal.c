#include "check.h"
#include "engine.h"

#include <inttypes.h>
#include <string.h>

static void record_codec(void)
{
    const unsigned char key[] = {0, 0xff};
    const unsigned char value[] = {0x80, 0};
    const FDBRecord record = {{key, 2}, {value, 2}, UINT64_C(0x1020304050607080),
                              UINT64_C(0x0102030405060708), FDB_PUT};
    const unsigned char expected[] = {
        2, 0, 0, 0, 2, 0, 0, 0, 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10,
        8, 7, 6, 5, 4, 3, 2, 1, 1, 0, 0, 0, 0, 0xff, 0x80, 0,
    };
    unsigned char bytes[sizeof(expected)];
    size_t length;
    STATUS(fdb_record_encode(&record, bytes, sizeof(bytes) - 1, &length), FASTDB_LIMIT_EXCEEDED);
    OK(fdb_record_encode(&record, bytes, sizeof(bytes), &length));
    CHECK(length == sizeof(bytes) && memcmp(bytes, expected, sizeof(bytes)) == 0);
    FDBRecord decoded;
    size_t consumed;
    OK(fdb_record_decode(bytes, length, 2, 2, &decoded, &consumed));
    CHECK(consumed == length && decoded.sequence == record.sequence);
    CHECK(decoded.expires_at_ms == record.expires_at_ms && decoded.kind == record.kind);
    CHECK(fdb_compare(decoded.key, record.key) == 0 && fdb_compare(decoded.value, record.value) == 0);
    for (size_t n = 0; n < length; ++n)
        STATUS(fdb_record_decode(bytes, n, 2, 2, &decoded, &consumed), FASTDB_CORRUPTION);
    STATUS(fdb_record_decode(bytes, length, 2, 1, &decoded, &consumed), FASTDB_CORRUPTION);
    bytes[27] = 1;
    STATUS(fdb_record_decode(bytes, length, 2, 2, &decoded, &consumed), FASTDB_CORRUPTION);
    bytes[27] = 0;
    bytes[24] = 99;
    STATUS(fdb_record_decode(bytes, length, 2, 2, &decoded, &consumed), FASTDB_CORRUPTION);
    fdb_encode_u32(bytes, UINT32_MAX);
    STATUS(fdb_record_decode(bytes, length, UINT32_MAX, 2, &decoded, &consumed), FASTDB_CORRUPTION);
    FDBRecord tombstone = {{key, sizeof(key)}, {0}, 9, 0, FDB_DELETE};
    OK(fdb_record_encode(&tombstone, bytes, sizeof(bytes), &length));
    OK(fdb_record_decode(bytes, length, 2, 0, &decoded, &consumed));
    CHECK(decoded.kind == FDB_DELETE && decoded.value.size == 0 && decoded.sequence == 9);
    tombstone.expires_at_ms = 1;
    STATUS(fdb_record_encode(&tombstone, bytes, sizeof(bytes), &length), FASTDB_INVALID_ARGUMENT);
    CHECK(fdb_compare((FastDBSlice){key, 1}, (FastDBSlice){key, 2}) < 0);
    CHECK(fdb_compare((FastDBSlice){key + 1, 1}, (FastDBSlice){value, 1}) > 0);
    CHECK(!fdb_add_size(SIZE_MAX, 1, &length));
}

static size_t check_node(const FDBNode *node, const FDBNode *min, const FDBNode *max,
                          size_t *count, size_t *bytes)
{
    if (node == NULL)
        return 1;
    CHECK(min == NULL || fdb_compare(min->record.key, node->record.key) < 0);
    CHECK(max == NULL || fdb_compare(max->record.key, node->record.key) > 0);
    CHECK(node->right == NULL || !node->right->red);
    CHECK(!node->red || node->left == NULL || !node->left->red);
    size_t left = check_node(node->left, min, node, count, bytes);
    size_t right = check_node(node->right, node, max, count, bytes);
    CHECK(left == right);
    ++*count;
    *bytes += sizeof(*node) + node->record.key.size + node->record.value.size;
    return left + (node->red ? 0U : 1U);
}

static void check_tree(const FDBTree *tree, size_t expected)
{
    size_t count = 0, bytes = 0;
    CHECK(tree->root == NULL || !tree->root->red);
    (void)check_node(tree->root, NULL, NULL, &count, &bytes);
    CHECK(count == expected && tree->count == count && tree->bytes == bytes);
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t r = *state;
    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    *state = r;
    return r;
}

static void exercise_tree(void)
{
    enum { KEYS = 257, STEPS = 65536 };
    uint32_t rng = UINT32_C(0x420e5b19);
    fprintf(stderr, "tree seed=%" PRIu32 ", operations=%d\n", rng, STEPS);
    uint32_t model[KEYS] = {0};
    size_t expected_count = 0;
    FDBTree tree = {0};
    for (uint64_t step = 1; step <= STEPS; ++step) {
        uint32_t index = next_random(&rng) % KEYS;
        unsigned char key[] = {(unsigned char)(index >> 8), (unsigned char)index};
        FastDBSlice slice = {key, sizeof(key)};
        uint32_t value = next_random(&rng) | 1;
        switch (value % 7) {
        case 0:
        case 1:
            fdb_tree_delete(&tree, slice);
            if (model[index] != 0)
                --expected_count;
            model[index] = 0;
            break;
        case 2:
        case 3: {
            const FDBNode *node = fdb_tree_get(&tree, slice);
            CHECK((node == NULL) == (model[index] == 0));
            if (node != NULL)
                CHECK(memcmp(node->record.value.data, &model[index], sizeof(value)) == 0);
            break;
        }
        default: {
            FDBRecord record = {slice, {&value, sizeof(value)}, step, 0, FDB_PUT};
            size_t bytes;
            CHECK(fdb_node_size(record.key, record.value, &bytes));
            FDBNode *node = fdb_node_create(&record, bytes);
            CHECK(node != NULL);
            fdb_tree_put(&tree, node);
            if (model[index] == 0)
                ++expected_count;
            model[index] = value;
            break;
        }
        }
        check_tree(&tree, expected_count);
    }
    const FDBNode *node = fdb_tree_bound(&tree, (FastDBSlice){0}, false);
    for (size_t index = 0; index < KEYS; ++index) {
        if (model[index] == 0)
            continue;
        unsigned char key[] = {(unsigned char)(index >> 8), (unsigned char)index};
        CHECK(node != NULL && fdb_compare(node->record.key, (FastDBSlice){key, sizeof(key)}) == 0);
        CHECK(memcmp(node->record.value.data, &model[index], sizeof(model[index])) == 0);
        node = fdb_tree_bound(&tree, node->record.key, true);
    }
    CHECK(node == NULL);
    fdb_tree_destroy(&tree);
    check_tree(&tree, 0);
}

static void sequence_limit(void)
{
    FastDB *db;
    OK(fastdb_open(NULL, &db));
    db->sequence = UINT64_MAX - 1;
    FastDBBatch *batch;
    OK(fastdb_batch_create(NULL, &batch));
    OK(fastdb_batch_put(batch, "a", 1, NULL, 0));
    OK(fastdb_batch_put(batch, "b", 1, NULL, 0));
    STATUS(fastdb_batch_commit(db, batch), FASTDB_LIMIT_EXCEEDED);
    CHECK(db->tree.count == 0 && db->sequence == UINT64_MAX - 1);
    fastdb_batch_destroy(batch);
    OK(fastdb_put(db, "c", 1, NULL, 0));
    STATUS(fastdb_delete(db, "c", 1), FASTDB_LIMIT_EXCEEDED);
    bool exists;
    OK(fastdb_exists(db, "c", 1, &exists));
    CHECK(exists && db->sequence == UINT64_MAX);
    OK(fastdb_close(db));
}

int main(void)
{
    record_codec();
    exercise_tree();
    sequence_limit();
}
