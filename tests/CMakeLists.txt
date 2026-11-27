function(fastdb_test name source library labels)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${library} fastdb_build_flags)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS "${labels}")
endfunction()

fastdb_test(fastdb_memory native/memory.c fastdb_shared integration)
fastdb_test(fastdb_internal native/internal.c fastdb_static unit)
target_include_directories(fastdb_internal PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
    "${PROJECT_SOURCE_DIR}/src/storage")
fastdb_test(fastdb_concurrency native/concurrency.c fastdb_static "integration;concurrency")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    fastdb_test(fastdb_alloc_failure native/alloc_failure.c fastdb_static "unit;fault")
    target_link_options(fastdb_alloc_failure PRIVATE -Wl,--wrap=malloc,--wrap=calloc)
    fastdb_test(fastdb_filesystem native/fs.c fastdb_static "integration;fault")
    target_include_directories(fastdb_filesystem PRIVATE "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_wal native/wal.c fastdb_static "integration;fault;recovery")
    target_include_directories(fastdb_wal PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_manifest native/manifest.c fastdb_static "integration;fault;recovery")
    target_include_directories(fastdb_manifest PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_wal_set native/wal_set.c fastdb_static "integration;fault;recovery")
    target_include_directories(fastdb_wal_set PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_sstable_writer native/sstable_writer.c fastdb_static "integration;fault")
    target_include_directories(fastdb_sstable_writer PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_sstable_reader native/sstable_reader.c fastdb_static "integration;fault")
    target_include_directories(fastdb_sstable_reader PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_flush native/flush.c fastdb_static "integration;fault;recovery")
    target_include_directories(fastdb_flush PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_crash native/crash.c fastdb_static "integration;recovery")
    target_include_directories(fastdb_crash PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_lsm native/lsm.c fastdb_shared "integration;recovery")
    fastdb_test(fastdb_persistent_memory native/persistent_memory.c fastdb_shared
        "integration;recovery")
    fastdb_test(fastdb_snapshot native/snapshot.c fastdb_shared "integration;concurrency;recovery")
    target_link_libraries(fastdb_snapshot PRIVATE Threads::Threads)
    fastdb_test(fastdb_ttl native/ttl.c fastdb_static "integration;recovery")
    fastdb_test(fastdb_eviction native/eviction.c fastdb_shared integration)
    target_link_options(fastdb_ttl PRIVATE -Wl,--wrap=fdb_read_time)
    target_include_directories(fastdb_ttl PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_lsm_fault native/lsm_fault.c fastdb_static "integration;fault;recovery")
    target_include_directories(fastdb_lsm_fault PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_public_crash native/public_crash.c fastdb_static "integration;recovery")
    target_include_directories(fastdb_public_crash PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_merge native/merge.c fastdb_static "integration;fault")
    target_include_directories(fastdb_merge PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_compaction_plan native/compaction_plan.c fastdb_static unit)
    target_include_directories(fastdb_compaction_plan PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_compaction native/compaction.c fastdb_static "integration;recovery")
    target_include_directories(fastdb_compaction PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_background_flush native/background_flush.c fastdb_static
        "integration;concurrency;recovery;fault")
    target_include_directories(fastdb_background_flush PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
    fastdb_test(fastdb_read_view native/read_view.c fastdb_static "integration;concurrency")
    target_include_directories(fastdb_read_view PRIVATE "${PROJECT_SOURCE_DIR}/src/core"
        "${PROJECT_SOURCE_DIR}/src/storage")
endif()

add_test(NAME fastdb_cli COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/cli.sh" $<TARGET_FILE:fastdb-cli>)
set_tests_properties(fastdb_cli PROPERTIES TIMEOUT 15 LABELS integration)
add_test(NAME fastdb_legacy_export COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/legacy_export.sh"
    $<TARGET_FILE:fastdb-legacy-export> $<TARGET_FILE:fastdb-cli>)
set_tests_properties(fastdb_legacy_export PROPERTIES TIMEOUT 20 LABELS integration)
