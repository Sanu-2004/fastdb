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
    FDBCompactionPolicy policy = {
        .levels = 3, .level0_file_trigger = 3, .level1_bytes = 100,
        .level_multiplier = 10, .max_inputs = 4,
    };
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
    FDBCompactionPolicy policy = {
        .levels = 4, .level0_file_trigger = 2, .level1_bytes = 100,
        .level_multiplier = 10, .max_inputs = 3,
    };
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

static void size_tiered_selection(void)
{
    FDBManifestEntry files[] = {
        table(1, 1, 1500, SLICE("a"), SLICE("b")),
        table(2, 0, 128, SLICE("b"), SLICE("e")),
        table(3, 0, 192, SLICE("c"), SLICE("f")),
        table(4, 0, 512, SLICE("x"), SLICE("y")),
        table(5, 2, 255, SLICE("x"), SLICE("z")),
        table(6, 0, UINT64_MAX, SLICE("m"), SLICE("o")),
        table(7, 0, UINT64_MAX - 1, SLICE("p"), SLICE("q")),
        {.id = 8, .kind = FDB_FILE_WAL},
    };
    FDBManifest manifest = {.mode = FDB_MODE_LSM, .entries = files,
                             .count = sizeof(files) / sizeof(files[0])};
    FDBCompactionPolicy policy = {
        .levels = 3, .level0_file_trigger = 3, .level1_bytes = 100,
        .level_multiplier = 10, .max_inputs = 2, .strategy = FASTDB_COMPACTION_SIZE_TIERED,
    };
    FDBCompactionPlan plan;
    size_t inputs[3] = {0, 0, SIZE_MAX};
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.count == 2 && plan.destination_level == 0 && plan.split_after.size == 0);
    CHECK(inputs[0] == 1 && inputs[1] == 2 && inputs[2] == SIZE_MAX);
    CHECK(plan.input_bytes == 320);
    policy.max_inputs = 3;
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.count == 3 && inputs[2] == 4 && plan.input_bytes == 575);

    policy.level0_file_trigger = 4;
    OK(fdb_compaction_plan(&manifest, &policy, false, NULL, inputs, &plan));
    CHECK(plan.count == 0);
    FastDBRange range = {.start = SLICE("y"), .end = SLICE("z"), .start_exclusive = true};
    OK(fdb_compaction_plan(&manifest, &policy, true, &range, inputs, &plan));
    CHECK(plan.count == 0);
    range.start_exclusive = false;
    OK(fdb_compaction_plan(&manifest, &policy, true, &range, inputs, &plan));
    CHECK(plan.count == 2 && inputs[0] == 3 && inputs[1] == 4 && plan.input_bytes == 767);
    range = (FastDBRange){.start = SLICE("m"), .end = SLICE("r")};
    OK(fdb_compaction_plan(&manifest, &policy, true, &range, inputs, &plan));
    CHECK(plan.count == 2 && inputs[0] == 5 && inputs[1] == 6);
    CHECK(plan.input_bytes == UINT64_MAX);
}

int main(void)
{
    closure_and_staging();
    levels_and_range();
    size_tiered_selection();
}
