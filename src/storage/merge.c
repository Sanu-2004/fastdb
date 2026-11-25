#include "merge.h"

#include <stdlib.h>

typedef struct Heap {
    FDBSSTIterator **items;
    size_t count;
} Heap;

static int internal_order(const FDBRecord *a, const FDBRecord *b)
{
    int keys = fdb_compare(a->key, b->key);
    if (keys != 0)
        return keys;
    return (a->sequence < b->sequence) - (a->sequence > b->sequence);
}

static int cursor_order(const FDBSSTIterator *a, const FDBSSTIterator *b)
{
    return internal_order(fdb_sst_iter_current(a), fdb_sst_iter_current(b));
}

static void push(Heap *heap, FDBSSTIterator *cursor)
{
    size_t pos = heap->count++;
    while (pos > 0) {
        size_t parent = (pos - 1) / 2;
        if (cursor_order(heap->items[parent], cursor) <= 0)
            break;
        heap->items[pos] = heap->items[parent];
        pos = parent;
    }
    heap->items[pos] = cursor;
}

static FDBSSTIterator *pop(Heap *heap)
{
    FDBSSTIterator *result = heap->items[0];
    FDBSSTIterator *last = heap->items[--heap->count];
    if (heap->count == 0)
        return result;
    size_t pos = 0;
    while (pos < heap->count / 2) {
        size_t child = pos * 2 + 1;
        if (child + 1 < heap->count && cursor_order(heap->items[child + 1], heap->items[child]) < 0)
            ++child;
        if (cursor_order(last, heap->items[child]) <= 0)
            break;
        heap->items[pos] = heap->items[child];
        pos = child;
    }
    heap->items[pos] = last;
    return result;
}

static FastDBStatus advance(Heap *heap, FDBSSTIterator *cursor)
{
    fdb_sst_iter_next(cursor);
    FastDBStatus status = fdb_sst_iter_status(cursor);
    if (status == FASTDB_OK && fdb_sst_iter_current(cursor) != NULL)
        push(heap, cursor);
    return status;
}

static FastDBStatus consume_duplicates(Heap *heap, const FDBRecord *winner)
{
    while (heap->count != 0) {
        const FDBRecord *record = fdb_sst_iter_current(heap->items[0]);
        if (internal_order(record, winner) != 0)
            break;
        if (record->kind != winner->kind || record->expires_at_ms != winner->expires_at_ms ||
            fdb_compare(record->value, winner->value) != 0)
            return FASTDB_CORRUPTION;
        FastDBStatus status = advance(heap, pop(heap));
        if (status != FASTDB_OK)
            return status;
    }
    return FASTDB_OK;
}

static FastDBStatus run(Heap *heap, FDBSSTVisit emit, void *context)
{
    while (heap->count != 0) {
        FDBSSTIterator *cursor = pop(heap);
        const FDBRecord *record = fdb_sst_iter_current(cursor);
        FastDBStatus status = consume_duplicates(heap, record);
        if (status == FASTDB_OK)
            status = emit(context, record);
        if (status == FASTDB_OK)
            status = advance(heap, cursor);
        if (status != FASTDB_OK)
            return status;
    }
    return FASTDB_OK;
}

static FastDBStatus check_budget(const FDBSSTReader *const *inputs, size_t count, size_t cap)
{
    size_t per_input = 2 * sizeof(FDBSSTIterator *);
    if (count > cap / per_input)
        return FASTDB_LIMIT_EXCEEDED;
    cap -= count * per_input;
    for (size_t i = 0; i < count; ++i) {
        if (inputs[i] == NULL)
            return FASTDB_INVALID_ARGUMENT;
        size_t estimate;
        if (!fdb_sst_reader_workspace(inputs[i], &estimate) || estimate > cap)
            return FASTDB_LIMIT_EXCEEDED;
        cap -= estimate;
    }
    return FASTDB_OK;
}

FastDBStatus fdb_sst_merge(const FDBSSTReader *const *inputs, size_t count,
                           size_t max_working_bytes, FDBSSTVisit emit, void *context)
{
    if (inputs == NULL || count == 0 || emit == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBStatus status = check_budget(inputs, count, max_working_bytes);
    if (status != FASTDB_OK)
        return status;
    FDBSSTIterator **cursors = calloc(count * 2, sizeof(*cursors));
    if (cursors == NULL)
        return FASTDB_OUT_OF_MEMORY;
    Heap heap = {.items = cursors + count};
    for (size_t i = 0; i < count && status == FASTDB_OK; ++i) {
        status = fdb_sst_iter_open(inputs[i], &cursors[i]);
        if (status == FASTDB_OK) {
            fdb_sst_iter_first(cursors[i]);
            status = fdb_sst_iter_status(cursors[i]);
            if (status == FASTDB_OK && fdb_sst_iter_current(cursors[i]) != NULL)
                push(&heap, cursors[i]);
        }
    }
    if (status == FASTDB_OK)
        status = run(&heap, emit, context);
    for (size_t i = 0; i < count; ++i)
        fdb_sst_iter_close(cursors[i]);
    free(cursors);
    return status;
}
