#include "check.h"
#include "compaction_plan.h"
#include "record.h"

#define SLICE(text) (FastDBSlice){(text), sizeof(text) - 1}

static FDBManifestEntry table(uint64_t id, uint32_t level, uint64_t bytes,
                               FastDBSlice min, FastDBSlice max)
{
    return (FDBManifestEntry){.id = id, .kind = FDB_FILE_SSTABLE, .level = level,
                              .bytes = bytes, .min_key = min, .max_key = max};
}

static void closure_and_staging(void)
{
    FDBManifestEntry files[] = {
        table(1, 0, 12, SLICE("a\0"), SLICE("h")),
        table(2, 0, 12, SLICE("c"), SLICE("k")),
        table(3, 1, 12, SLICE("g"), SLICE("m")),
        table(4, 1, 12, SLICE("0"), SLICE("a\0")),
        table(5, 1, 12, SLICE("c"), SLICE("e")),
        table(6, 1, 12, SLICE("z"), SLICE("z")),
        {.id = 7, .kind = FDB_FILE_WAL},
    };
    FDBManifest manifest = {.mode = FDB_MODE_LSM, .entries = files,
                             .count = sizeof(files) / sizeof(files[0])};
    FDBCompactionPolicy policy = {3, 3, 100, 10, 4};
    FDBCompactionPlan plan;
    size_t inputs[4];
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.count == 0);
    policy.level0_file_trigger = 2;
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.source_level == 0 && plan.destination_level == 1 && plan.count == 4);
    CHECK(inputs[0] == 0 && inputs[1] == 2 && inputs[2] == 3 && inputs[3] == 4);
    CHECK(plan.split_after.size == 0);
    policy.max_inputs = 2;
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.count == 2 && inputs[0] == 0 && inputs[1] == 3);
    CHECK(fdb_compare(plan.split_after, SLICE("a\0")) == 0);
    files[5].min_key = SLICE("m");
    STATUS(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan), FASTDB_CORRUPTION);
    CHECK(plan.count == 0);
}

static void levels_and_range(void)
{
    FDBManifestEntry files[] = {
        table(1, 0, 20, SLICE("a"), SLICE("b")),
        table(2, 0, 40, SLICE("d"), SLICE("e")),
        table(3, 1, 100, SLICE("c"), SLICE("d")),
        table(4, 2, 1001, SLICE("f"), SLICE("g")),
        table(5, 3, UINT64_MAX, SLICE("h"), SLICE("i")),
    };
    FDBManifest manifest = {.mode = FDB_MODE_LSM, .entries = files,
                             .count = sizeof(files) / sizeof(files[0])};
    FDBCompactionPolicy policy = {4, 2, 100, 10, 3};
    FDBCompactionPlan plan;
    size_t inputs[3];
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.source_level == 2 && plan.destination_level == 3 && plan.count == 1);
    files[3].bytes = 1000;
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.source_level == 0 && plan.destination_level == 1 && inputs[0] == 0);
    files[2].bytes = 101;
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.source_level == 1 && plan.destination_level == 2 && inputs[0] == 2);

    FastDBRange range = {.start = SLICE("b"), .end = SLICE("c"), .start_exclusive = true};
    OK(fdb_compaction_plan(&manifest, &policy, true, &range, inputs, &plan));
    CHECK(plan.count == 0);
    range.start_exclusive = false;
    OK(fdb_compaction_plan(&manifest, &policy, true, &range, inputs, &plan));
    CHECK(plan.count == 1 && inputs[0] == 0);
    range.start = SLICE("h");
    range.end = (FastDBSlice){0};
    OK(fdb_compaction_plan(&manifest, &policy, true, &range, inputs, &plan));
    CHECK(plan.count == 0);
    policy.level0_file_trigger = 10;
    policy.level1_bytes = UINT64_MAX / 2;
    files[3].bytes = UINT64_MAX;
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.count == 0);
    policy.levels = 3;
    STATUS(fdb_compaction_plan(&manifest, &policy, true, NULL, inputs, &plan), FASTDB_CORRUPTION);
}

int main(void)
{
    closure_and_staging();
    levels_and_range();
}
