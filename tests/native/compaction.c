#include "disk_fixture.h"
#include "engine.h"
#include "manifest.h"
#include "recovery.h"
#include "sstable_format.h"

static uint64_t check_levels(const char *path, const size_t *counts, uint32_t levels)
{
    FDBDirectory directory;
    OK(fdb_dir_lock(path, NULL, &directory));
    const FDBManifestLimits limits = {16, 2048, 32, levels};
    FDBManifestLoad catalog;
    OK(fdb_manifest_load(&directory, &limits, &catalog));
    for (uint32_t level = 0; level < levels; ++level) {
        size_t actual = 0;
        for (size_t i = 0; i < catalog.view.count; ++i) {
            const FDBManifestEntry *entry = &catalog.view.entries[i];
            if (entry->kind == FDB_FILE_SSTABLE && entry->level == level)
                ++actual;
        }
        CHECK(actual == counts[level]);
    }
    uint64_t generation = catalog.view.generation;
    fdb_manifest_release(&catalog);
    OK(fdb_dir_close(&directory));
    return generation;
}

static void check_single_table(const char *path, const FastDBOptions *options, uint32_t level,
                               uint64_t records, uint64_t tombstones, uint64_t max_sequence)
{
    FDBDirectory directory;
    OK(fdb_dir_lock(path, NULL, &directory));
    const FDBManifestLimits manifest_limits = {16, 2048, 32, 2};
    FDBManifestLoad catalog;
    OK(fdb_manifest_load(&directory, &manifest_limits, &catalog));
    const FastDBPersistentOptions *storage = &options->storage;
    const FDBSSTLimits limits = {
        .max_key_bytes = options->limits.max_key_bytes,
        .max_value_bytes = options->limits.max_value_bytes,
        .target_block_bytes = storage->target_block_bytes,
        .max_block_bytes = FDB_RECORD_HEADER_BYTES + SST_BLOCK_HEADER_BYTES + SST_OFFSET_BYTES +
                           options->limits.max_key_bytes + options->limits.max_value_bytes,
        .max_index_bytes = storage->max_index_bytes,
        .max_filter_bytes = storage->max_filter_bytes,
        .bloom_bits_per_key = storage->bloom_bits_per_key,
    };
    size_t tables = 0;
    for (size_t i = 0; i < catalog.view.count; ++i) {
        const FDBManifestEntry *entry = &catalog.view.entries[i];
        if (entry->kind != FDB_FILE_SSTABLE || entry->level != level)
            continue;
        ++tables;
        FDBSSTReader *reader;
        OK(fdb_catalog_open_table(&directory, entry, &limits, &reader));
        const FDBSSTProperties *properties = fdb_sst_reader_properties(reader);
        CHECK(properties->records == records && properties->tombstones == tombstones);
        CHECK(properties->max_sequence == max_sequence);
        OK(fdb_sst_reader_close(reader));
    }
    CHECK(tables == 1);
    fdb_manifest_release(&catalog);
    OK(fdb_dir_close(&directory));
}

static void reclaim_shadowed_versions(void)
{
    char path[] = "/tmp/fastdb-reclaim-versions.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "old", 3));
    OK(fastdb_put(db, "m", 1, "old", 3));
    OK(fastdb_put(db, "z", 1, "guard", 5));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    OK(fastdb_put(db, "a", 1, "new", 3));
    OK(fastdb_delete(db, "m", 1));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"new", 3}, 4);
    expect_missing(db, (FastDBSlice){"m", 1});
    void *value;
    size_t size;
    uint64_t revision;
    OK(fastdb_snapshot_get(snapshot, "m", 1, &value, &size, &revision));
    CHECK(size == 3 && memcmp(value, "old", 3) == 0 && revision == 2);
    fastdb_free(value);
    OK(fastdb_snapshot_get(snapshot, "a", 1, &value, &size, &revision));
    CHECK(size == 3 && memcmp(value, "old", 3) == 0 && revision == 1);
    fastdb_free(value);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 1}, 2);
    check_single_table(path, options, 1, 3, 1, 5);

    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"m", 1});
    expect_value(db, (FastDBSlice){"z", 1}, (FastDBSlice){"guard", 5}, 3);
    OK(fastdb_put(db, "m", 1, "reinserted", 10));
    expect_value(db, (FastDBSlice){"m", 1}, (FastDBSlice){"reinserted", 10}, 6);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static FastDBOptions *bottommost_options(const char *path)
{
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));
    return options;
}

static void reclaim_tombstones_except_coverage(void)
{
    char path[] = "/tmp/fastdb-reclaim-tombstones.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = bottommost_options(path);
    FastDB *db;
    OK(fastdb_open(options, &db));
    for (char key = 'a'; key <= 'c'; ++key)
        OK(fastdb_put(db, &key, 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    for (char key = 'a'; key <= 'c'; ++key)
        OK(fastdb_delete(db, &key, 1));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    for (char key = 'a'; key <= 'c'; ++key)
        expect_missing(db, (FastDBSlice){&key, 1});
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 1}, 2);
    check_single_table(path, options, 1, 1, 1, 6);

    OK(fastdb_open(options, &db));
    for (char key = 'a'; key <= 'c'; ++key)
        expect_missing(db, (FastDBSlice){&key, 1});
    OK(fastdb_put(db, "a", 1, "new", 3));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"new", 3}, 7);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void retain_mask_over_unselected_input(void)
{
    char path[] = "/tmp/fastdb-mask-unselected.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = bottommost_options(path);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_delete(db, "a", 1));
    OK(fastdb_put(db, "z", 1, "guard", 5));
    OK(fastdb_flush(db));
    const FastDBRange range = {.start = {"z", 1}};
    OK(fastdb_compact_range(db, &range));
    expect_missing(db, (FastDBSlice){"a", 1});
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){1, 1}, 2);
    check_single_table(path, options, 1, 2, 1, 3);

    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"a", 1});
    OK(fastdb_compact(db));
    expect_missing(db, (FastDBSlice){"a", 1});
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 1}, 2);
    check_single_table(path, options, 1, 1, 0, 3);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void retain_mask_during_snapshot(void)
{
    char path[] = "/tmp/fastdb-mask-snapshot.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = bottommost_options(path);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    OK(fastdb_delete(db, "a", 1));
    OK(fastdb_put(db, "z", 1, "guard", 5));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    expect_missing(db, (FastDBSlice){"a", 1});
    void *value;
    size_t size;
    uint64_t revision;
    OK(fastdb_snapshot_get(snapshot, "a", 1, &value, &size, &revision));
    CHECK(size == 3 && memcmp(value, "old", 3) == 0 && revision == 1);
    fastdb_free(value);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 1}, 2);
    check_single_table(path, options, 1, 2, 1, 3);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void staged_and_reopen(void)
{
    char path[] = "/tmp/fastdb-compaction-stage.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions compaction;
    fastdb_compaction_options_default(&compaction);
    compaction.max_levels = 2;
    compaction.max_merge_inputs = 2;
    OK(fastdb_options_set_compaction(options, &compaction));

    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "d", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_put(db, "b", 1, "old", 3));
    OK(fastdb_flush(db));
    FastDBRange range = {.start = {"b", 1}, .end = {"d", 1}};
    OK(fastdb_compact_range(db, &range));
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){1, 1}, 2);
    OK(fastdb_open(options, &db));
    OK(fastdb_compact(db));
    OK(fastdb_put(db, "a", 1, "a", 1));
    OK(fastdb_put(db, "b", 1, "new", 3));
    OK(fastdb_put(db, "c", 1, "c", 1));
    OK(fastdb_delete(db, "d", 1));
    OK(fastdb_put(db, "e", 1, "e", 1));
    OK(fastdb_flush(db));
    OK(fastdb_put(db, "wal", 3, "first", 5));
    range.start = (FastDBSlice){"z", 1};
    STATUS(fastdb_compact_range(db, &range), FASTDB_INVALID_ARGUMENT);
    range.start = range.end;
    OK(fastdb_compact_range(db, &range));
    OK(fastdb_compact(db));
    OK(fastdb_put(db, "wal", 3, "second", 6));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"a", 1}, 3);
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){"new", 3}, 4);
    expect_value(db, (FastDBSlice){"c", 1}, (FastDBSlice){"c", 1}, 5);
    expect_missing(db, (FastDBSlice){"d", 1});
    expect_value(db, (FastDBSlice){"e", 1}, (FastDBSlice){"e", 1}, 7);
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 2}, 2);

    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){"new", 3}, 4);
    expect_missing(db, (FastDBSlice){"d", 1});
    expect_value(db, (FastDBSlice){"wal", 3}, (FastDBSlice){"second", 6}, 9);
    OK(fastdb_put(db, "d", 1, NULL, 0));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    expect_value(db, (FastDBSlice){"d", 1}, (FastDBSlice){0}, 10);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void workspace_limits_fan_in(void)
{
    char path[] = "/tmp/fastdb-compaction-fan-in.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions compaction;
    fastdb_compaction_options_default(&compaction);
    compaction.max_levels = 2;
    compaction.max_merge_inputs = 3;
    OK(fastdb_options_set_compaction(options, &compaction));

    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    OK(fastdb_put(db, "c", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    OK(fastdb_put(db, "a", 1, "new", 3));
    OK(fastdb_delete(db, "c", 1));
    OK(fastdb_flush(db));
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){1, 2}, 2);

    size_t block = FDB_RECORD_HEADER_BYTES + options->limits.max_key_bytes +
                   options->limits.max_value_bytes + SST_BLOCK_HEADER_BYTES + SST_OFFSET_BYTES;
    FDBSSTLimits table = {
        options->limits.max_key_bytes, options->limits.max_value_bytes,
        options->storage.target_block_bytes, block, options->storage.max_index_bytes,
        options->storage.max_filter_bytes, options->storage.bloom_bits_per_key,
    };
    size_t reader, writer;
    CHECK(fdb_sst_reader_budget(&table, &reader));
    CHECK(fdb_sst_writer_budget(&table, &writer));
    size_t input = reader + sizeof(FDBSSTReader *) + 2 * sizeof(FDBSSTIterator *) +
                   2 * sizeof(size_t) + sizeof(uint64_t);
    /* One byte below three inputs with selection arrays. Two inputs still fit. */
    compaction.max_working_bytes = writer + 3 * input - 1;
    OK(fastdb_options_set_compaction(options, &compaction));
    OK(fastdb_open(options, &db));
    OK(fastdb_compact(db));
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 2}, 2);

    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"new", 3}, 3);
    expect_missing(db, (FastDBSlice){"c", 1});
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void auto_cascade_and_disable(void)
{
    char path[] = "/tmp/fastdb-compaction-auto.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions compaction;
    fastdb_compaction_options_default(&compaction);
    compaction.max_levels = 3;
    compaction.level0_file_trigger = 2;
    compaction.level1_bytes = 1;
    compaction.max_merge_inputs = 1;
    STATUS(fastdb_options_set_compaction(options, &compaction), FASTDB_INVALID_ARGUMENT);
    compaction.max_merge_inputs = 2;
    OK(fastdb_options_set_compaction(options, &compaction));
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "x", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_put(db, "x", 1, "new", 3));
    OK(fastdb_flush(db));
    expect_value(db, (FastDBSlice){"x", 1}, (FastDBSlice){"new", 3}, 2);
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){1, 0, 1}, 3);

    compaction.enabled = false;
    OK(fastdb_options_set_compaction(options, &compaction));
    OK(fastdb_open(options, &db));
    OK(fastdb_delete(db, "x", 1));
    OK(fastdb_flush(db));
    STATUS(fastdb_compact(db), FASTDB_UNSUPPORTED);
    expect_missing(db, (FastDBSlice){"x", 1});
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){2, 0, 1}, 3);

    compaction.enabled = true;
    OK(fastdb_options_set_compaction(options, &compaction));
    OK(fastdb_open(options, &db));
    OK(fastdb_compact(db));
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 0, 1}, 3);
    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"x", 1});
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void workspace_limit(void)
{
    char path[] = "/tmp/fastdb-compaction-budget.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions compaction;
    fastdb_compaction_options_default(&compaction);
    compaction.max_levels = 2;
    compaction.max_working_bytes = 1;
    OK(fastdb_options_set_compaction(options, &compaction));
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_compact(db));
    OK(fastdb_put(db, "key", 3, NULL, 0));
    OK(fastdb_flush(db));
    STATUS(fastdb_compact(db), FASTDB_LIMIT_EXCEEDED);
    expect_value(db, (FastDBSlice){"key", 3}, (FastDBSlice){0}, 1);
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){1, 0}, 2);
    compaction.max_working_bytes = 0;
    OK(fastdb_options_set_compaction(options, &compaction));
    OK(fastdb_open(options, &db));
    OK(fastdb_compact(db));
    OK(fastdb_close(db));
    check_levels(path, (size_t[]){0, 1}, 2);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void reserve_split_outputs_in_one_edit(void)
{
    char path[] = "/tmp/fastdb-compact-ids.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions compaction;
    fastdb_compaction_options_default(&compaction);
    compaction.max_levels = 2;
    compaction.target_file_bytes = 1;
    OK(fastdb_options_set_compaction(options, &compaction));

    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "a", 1));
    OK(fastdb_put(db, "b", 1, "b", 1));
    OK(fastdb_put(db, "c", 1, "c", 1));
    OK(fastdb_flush(db));
    OK(fastdb_close(db));
    uint64_t before = check_levels(path, (size_t[]){1, 0}, 2);
    OK(fastdb_open(options, &db));
    OK(fastdb_compact(db));
    OK(fastdb_close(db));
    uint64_t after = check_levels(path, (size_t[]){0, 3}, 2);
    CHECK(after == before + 2);

    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"a", 1}, 1);
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){"b", 1}, 2);
    expect_value(db, (FastDBSlice){"c", 1}, (FastDBSlice){"c", 1}, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    reclaim_shadowed_versions();
    retain_mask_over_unselected_input();
    retain_mask_during_snapshot();
    reclaim_tombstones_except_coverage();
    workspace_limits_fan_in();
    staged_and_reopen();
    auto_cascade_and_disable();
    workspace_limit();
    reserve_split_outputs_in_one_edit();
}
