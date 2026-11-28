#include "check.h"
#include "merge.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const FDBSSTLimits limits = {12, 128, 64, 256, 1024, 256, 10, false, 0};

static const FDBRecord older[] = {
    {{"a\0", 2}, {"old", 3}, 9, 100, FDB_PUT},
    {{"b", 1}, {0}, 5, 0, FDB_DELETE},
    {{"c", 1}, {"keep", 4}, 8, 0, FDB_PUT},
};

static const FDBRecord newer[] = {
    {{"a\0", 2}, {"old", 3}, 9, 100, FDB_PUT},
    {{"a\0", 2}, {0}, 4, 0, FDB_PUT},
    {{"b", 1}, {"new", 3}, 7, 0, FDB_PUT},
};

static const FDBRecord third[] = {
    {{"a\0", 2}, {0}, 12, 0, FDB_DELETE},
    {{"c", 1}, {"first", 5}, 1, 0, FDB_PUT},
};

static FDBFsFault inject_read(void *context, FDBFsOp op, size_t requested)
{
    (void)requested;
    size_t *attempt = context;
    if (op == FDB_FS_READ && *attempt != 0 && --*attempt == 0)
        return (FDBFsFault){.error = EIO};
    return (FDBFsFault){0};
}

static FDBSSTReader *table(const FDBDirectory *dir, const char *name, uint64_t id,
                           const FDBRecord *rows, size_t count)
{
    FDBSSTWriter *writer;
    OK(fdb_sst_writer_open(dir, name, id, &limits, &writer));
    for (size_t i = 0; i < count; ++i)
        OK(fdb_sst_writer_add(writer, &rows[i]));
    FDBSSTProperties props;
    OK(fdb_sst_writer_finish(writer, &props));
    OK(fdb_sst_writer_destroy(writer));
    FDBSSTReader *reader;
    OK(fdb_sst_reader_open(dir, name, id, &limits, &reader));
    return reader;
}

typedef struct Verify {
    const FDBRecord *const *rows;
    size_t count;
    size_t offset;
} Verify;

static FastDBStatus verify_row(void *context, const FDBRecord *record)
{
    Verify *v = context;
    CHECK(v->offset < v->count);
    const FDBRecord *expected = v->rows[v->offset++];
    CHECK(fdb_compare(record->key, expected->key) == 0);
    CHECK(record->sequence == expected->sequence && record->kind == expected->kind);
    CHECK(record->expires_at_ms == expected->expires_at_ms);
    CHECK(fdb_compare(record->value, expected->value) == 0);
    return FASTDB_OK;
}

static FastDBStatus write_row(void *context, const FDBRecord *record)
{
    return fdb_sst_writer_add(context, record);
}

typedef struct Split {
    uint64_t next;
    size_t reserved;
    size_t finished;
    char name[3][32];
    uint64_t records[3];
    unsigned char expected_key[3];
} Split;

static FastDBStatus reserve_file(void *context, uint64_t *id)
{
    Split *split = context;
    *id = split->next++;
    ++split->reserved;
    return FASTDB_OK;
}

static FastDBStatus finished_file(void *context, const char *name, const FDBSSTProperties *props,
                                   bool suffix)
{
    Split *split = context;
    CHECK(split->finished < 3 && !suffix);
    size_t i = split->finished++;
    unsigned char expected = split->expected_key[i];
    CHECK(strlen(name) < sizeof(split->name[i]));
    strcpy(split->name[i], name);
    CHECK(props->min_key.size != 0 && ((const unsigned char *)props->min_key.data)[0] == expected);
    CHECK(fdb_compare(props->min_key, props->max_key) == 0);
    CHECK(props->file_bytes > 200);
    split->records[i] = props->records;
    return FASTDB_OK;
}

static void split_merge(const FDBDirectory *dir, const FDBSSTReader *const *inputs, size_t budget)
{
    size_t output_budget;
    CHECK(fdb_sst_writer_budget(&limits, &output_budget));
    FDBMergeLimits options = {.output_table = limits, .max_inputs = 3, .max_outputs = 3,
                               .max_working_bytes = budget + output_budget,
                               .target_output_bytes = 200};
    Split state = {.next = 10, .expected_key = {'a', 'b', 'c'}};
    const FDBMergeOutput output = {reserve_file, finished_file, &state};
    --options.max_inputs;
    STATUS(fdb_sst_merge_split(dir, inputs, 3, &options, &output), FASTDB_LIMIT_EXCEEDED);
    ++options.max_inputs;
    --options.max_working_bytes;
    STATUS(fdb_sst_merge_split(dir, inputs, 3, &options, &output), FASTDB_LIMIT_EXCEEDED);
    CHECK(state.reserved == 0);
    ++options.max_working_bytes;
    OK(fdb_sst_merge_split(dir, inputs, 3, &options, &output));
    CHECK(state.finished == 3 && state.reserved == 3);
    CHECK(state.records[0] == 3 && state.records[1] == 2 && state.records[2] == 2);
    for (size_t i = 0; i < 3; ++i) {
        FDBSSTReader *reader;
        OK(fdb_sst_reader_open(dir, state.name[i], 10 + i, &limits, &reader));
        OK(fdb_sst_reader_verify(reader));
        OK(fdb_sst_reader_close(reader));
        OK(fdb_dir_unlink(dir, state.name[i]));
    }

    options.max_outputs = 1;
    state = (Split){.next = 20, .expected_key = {'a', 'b', 'c'}};
    STATUS(fdb_sst_merge_split(dir, inputs, 3, &options, &output), FASTDB_LIMIT_EXCEEDED);
    CHECK(state.reserved == 1 && state.finished == 1);
    OK(fdb_dir_unlink(dir, state.name[0]));

    options.output_table.max_index_bytes = 54;
    state = (Split){.next = 30, .expected_key = {'a', 'b', 'c'}};
    STATUS(fdb_sst_merge_split(dir, inputs, 3, &options, &output), FASTDB_LIMIT_EXCEEDED);
    CHECK(state.reserved == 1 && state.finished == 0);
    OK(fdb_dir_unlink(dir, ".sst-00000000000000000030.tmp"));
}

static void test_merge(const FDBDirectory *dir, size_t *fault)
{
    FDBSSTReader *a = table(dir, "older", 1, older, 3);
    FDBSSTReader *b = table(dir, "newer", 2, newer, 3);
    FDBSSTReader *c = table(dir, "third", 3, third, 2);
    const FDBSSTReader *inputs[] = {b, a, c};
    const FDBRecord *two_rows[] = {&older[0], &newer[1], &newer[2], &older[1], &older[2]};
    Verify v = {two_rows, sizeof(two_rows) / sizeof(two_rows[0]), 0};
    size_t per_reader;
    CHECK(fdb_sst_reader_workspace(a, &per_reader));
    size_t budget = 3 * (per_reader + 2 * sizeof(void *));
    split_merge(dir, inputs, budget);
    STATUS(fdb_sst_merge(inputs, 3, budget - 1, verify_row, &v), FASTDB_LIMIT_EXCEEDED);
    CHECK(v.offset == 0);
    OK(fdb_sst_merge(inputs, 2, budget, verify_row, &v));
    CHECK(v.offset == v.count);

    FDBSSTWriter *output;
    OK(fdb_sst_writer_open(dir, "merged", 4, &limits, &output));
    OK(fdb_sst_merge(inputs, 3, budget, write_row, output));
    FDBSSTProperties props;
    OK(fdb_sst_writer_finish(output, &props));
    CHECK(props.records == 7 && props.tombstones == 2);
    OK(fdb_sst_writer_destroy(output));
    FDBSSTReader *merged;
    OK(fdb_sst_reader_open(dir, "merged", 4, &limits, &merged));
    const FDBRecord *three_rows[] = {&third[0], &older[0], &newer[1], &newer[2],
                                     &older[1], &older[2], &third[1]};
    v = (Verify){three_rows, sizeof(three_rows) / sizeof(three_rows[0]), 0};
    OK(fdb_sst_reader_walk(merged, verify_row, &v));
    CHECK(v.offset == v.count);
    OK(fdb_sst_reader_close(merged));

    *fault = 4;
    v.offset = 0;
    STATUS(fdb_sst_merge(inputs, 3, budget, verify_row, &v), FASTDB_IO_ERROR);
    CHECK(v.offset == 1 && *fault == 0);
    OK(fdb_sst_reader_verify(a));
    OK(fdb_sst_reader_verify(b));
    OK(fdb_sst_reader_verify(c));

    FDBRecord conflicting = newer[0];
    conflicting.value = (FastDBSlice){"conflict", 8};
    FDBSSTReader *bad = table(dir, "conflicting", 5, &conflicting, 1);
    const FDBSSTReader *same_revision[] = {a, bad};
    v.offset = 0;
    STATUS(fdb_sst_merge(same_revision, 2, budget, verify_row, &v), FASTDB_CORRUPTION);
    CHECK(v.offset == 0);
    OK(fdb_sst_reader_close(bad));
    OK(fdb_sst_reader_close(c));
    OK(fdb_sst_reader_close(b));
    OK(fdb_sst_reader_close(a));
}

int main(void)
{
    char path[] = "/tmp/fastdb-merge.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    size_t fault = 0;
    const FDBFs fs = {inject_read, &fault};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    test_merge(&dir, &fault);
    const char *files[] = {"older", "newer", "third", "merged", "conflicting"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        OK(fdb_dir_unlink(&dir, files[i]));
    OK(fdb_dir_close(&dir));
    char lock[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock, sizeof(lock), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock) == 0 && rmdir(path) == 0);
}
