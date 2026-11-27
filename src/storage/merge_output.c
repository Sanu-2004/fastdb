#include "merge.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct OutputTables {
    const FDBDirectory *dir;
    const FDBMergeLimits *limits;
    const FDBMergeOutput *output;
    FDBSSTWriter *writer;
    size_t files;
    bool suffix;
    unsigned char *dropped_key;
    size_t dropped_key_size;
    char temp[sizeof(".sst-18446744073709551615.tmp")];
} OutputTables;

static FastDBStatus begin_table(OutputTables *tables)
{
    if (tables->files == tables->limits->max_outputs)
        return FASTDB_LIMIT_EXCEEDED;
    uint64_t id;
    FastDBStatus status = tables->output->reserve(tables->output->context, &id);
    if (status != FASTDB_OK)
        return status;
    (void)snprintf(tables->temp, sizeof(tables->temp), ".sst-%020" PRIu64 ".tmp", id);
    status = fdb_sst_writer_open(tables->dir, tables->temp, id,
                                  &tables->limits->output_table, &tables->writer);
    if (status == FASTDB_OK)
        ++tables->files;
    return status;
}

static FastDBStatus finish_table(OutputTables *tables)
{
    FDBSSTProperties props;
    FastDBStatus status = fdb_sst_writer_finish(tables->writer, &props);
    if (status == FASTDB_OK)
        status = tables->output->finished(tables->output->context, tables->temp, &props,
                                          tables->suffix);
    FastDBStatus close_status = fdb_sst_writer_destroy(tables->writer);
    tables->writer = NULL;
    return status == FASTDB_OK ? close_status : status;
}

static FastDBStatus switch_table(OutputTables *tables)
{
    FastDBStatus status = finish_table(tables);
    return status == FASTDB_OK ? begin_table(tables) : status;
}

static bool starts_new_key(const FDBSSTWriter *writer, FastDBSlice key)
{
    FastDBSlice previous = fdb_sst_writer_last_key(writer);
    return previous.size != 0 && fdb_compare(previous, key) != 0;
}

static FastDBStatus emit(void *context, const FDBRecord *record)
{
    OutputTables *tables = context;
    if (tables->dropped_key_size != 0) {
        FastDBSlice dropped = {tables->dropped_key, tables->dropped_key_size};
        if (fdb_compare(dropped, record->key) == 0)
            return FASTDB_OK;
        tables->dropped_key_size = 0;
    }
    /* Merge order is newest first. The writer keeps each key group together. */
    bool same_key = tables->writer != NULL &&
                    fdb_compare(fdb_sst_writer_last_key(tables->writer), record->key) == 0;
    if (tables->limits->discard_shadowed_puts && same_key && record->kind == FDB_PUT)
        return FASTDB_OK;
    if (tables->dropped_key != NULL && !same_key && record->kind == FDB_DELETE &&
        record->sequence < tables->limits->gc_anchor) {
        /* Suppress the entire key group so an older PUT cannot resurface. */
        memcpy(tables->dropped_key, record->key.data, record->key.size);
        tables->dropped_key_size = record->key.size;
        return FASTDB_OK;
    }
    FastDBStatus status = FASTDB_OK;
    FastDBSlice boundary = tables->limits->split_after;
    bool suffix = boundary.size != 0 && fdb_compare(record->key, boundary) > 0;
    if (tables->writer != NULL && suffix != tables->suffix)
        status = finish_table(tables);
    tables->suffix = suffix;
    if (status != FASTDB_OK)
        return status;
    if (tables->writer == NULL)
        status = begin_table(tables);
    else if (starts_new_key(tables->writer, record->key) &&
             fdb_sst_writer_file_bytes(tables->writer) >= tables->limits->target_output_bytes)
        status = switch_table(tables);
    if (status != FASTDB_OK)
        return status;
    status = fdb_sst_writer_add(tables->writer, record);
    if (status == FASTDB_LIMIT_EXCEEDED && starts_new_key(tables->writer, record->key)) {
        status = switch_table(tables);
        if (status == FASTDB_OK)
            status = fdb_sst_writer_add(tables->writer, record);
    }
    return status;
}

FastDBStatus fdb_sst_merge_split(const FDBDirectory *dir, const FDBSSTReader *const *inputs,
                                 size_t count, const FDBMergeLimits *limits,
                                 const FDBMergeOutput *output)
{
    if (dir == NULL || limits == NULL || output == NULL || output->reserve == NULL ||
        output->finished == NULL || limits->max_inputs == 0 || limits->max_outputs == 0 ||
        limits->target_output_bytes == 0 || !fdb_sst_valid_limits(&limits->output_table) ||
        (limits->split_after.size != 0 && limits->split_after.data == NULL))
        return FASTDB_INVALID_ARGUMENT;
    size_t writer_bytes;
    if (count > limits->max_inputs ||
        !fdb_sst_writer_budget(&limits->output_table, &writer_bytes) ||
        writer_bytes > limits->max_working_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    size_t workspace = limits->max_working_bytes - writer_bytes;
    OutputTables tables = {.dir = dir, .limits = limits, .output = output};
    if (limits->gc_anchor != 0) {
        size_t key_bytes = limits->output_table.max_key_bytes;
        if (key_bytes > workspace)
            return FASTDB_LIMIT_EXCEEDED;
        tables.dropped_key = malloc(key_bytes);
        if (tables.dropped_key == NULL)
            return FASTDB_OUT_OF_MEMORY;
        workspace -= key_bytes;
    }
    FastDBStatus status = fdb_sst_merge(inputs, count, workspace, emit, &tables);
    free(tables.dropped_key);
    if (status == FASTDB_OK && tables.writer != NULL)
        return finish_table(&tables);
    (void)fdb_sst_writer_destroy(tables.writer);
    return status;
}
