cmake_minimum_required(VERSION 3.20)
project(FastDB VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(FASTDB_SANITIZER "none" CACHE STRING "Instrumentation: none, address, or thread")
set_property(CACHE FASTDB_SANITIZER PROPERTY STRINGS none address thread)
add_library(fastdb_build_flags INTERFACE)
target_compile_definitions(fastdb_build_flags INTERFACE
    _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64
    FASTDB_VERSION_STRING="${PROJECT_VERSION}")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(fastdb_build_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
        -Wmissing-prototypes -Werror)
endif()
if(FASTDB_SANITIZER STREQUAL "address")
    set(fastdb_sanitize_flags -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif(FASTDB_SANITIZER STREQUAL "thread")
    set(fastdb_sanitize_flags -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT FASTDB_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unknown FASTDB_SANITIZER: ${FASTDB_SANITIZER}")
endif()
if(fastdb_sanitize_flags)
    target_compile_options(fastdb_build_flags INTERFACE ${fastdb_sanitize_flags})
    target_link_options(fastdb_build_flags INTERFACE ${fastdb_sanitize_flags})
endif()

add_library(fastdb_objects OBJECT
    src/core/disk.c src/core/engine.c src/core/memtable.c src/core/record.c src/core/scan.c
    src/core/snapshot.c
    src/storage/block_cache.c src/storage/bloom.c src/storage/compaction.c
    src/storage/compaction_plan.c src/storage/crc32c.c
    src/storage/flush.c src/storage/fs.c
    src/storage/manifest.c src/storage/merge.c src/storage/merge_output.c
    src/storage/recovery.c src/storage/wal.c
    src/storage/wal_set.c src/storage/sstable_reader.c src/storage/sstable_writer.c)
target_include_directories(fastdb_objects PRIVATE include src src/core src/storage)
target_link_libraries(fastdb_objects PRIVATE fastdb_build_flags Threads::Threads)
set_target_properties(fastdb_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

add_library(fastdb_static STATIC $<TARGET_OBJECTS:fastdb_objects>)
add_library(fastdb_shared SHARED $<TARGET_OBJECTS:fastdb_objects>)
add_library(FastDB::static ALIAS fastdb_static)
add_library(FastDB::shared ALIAS fastdb_shared)
foreach(target IN ITEMS fastdb_static fastdb_shared)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME fastdb)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
endforeach()
set_target_properties(fastdb_static PROPERTIES EXPORT_NAME static)
set_target_properties(fastdb_shared PROPERTIES EXPORT_NAME shared
    VERSION ${PROJECT_VERSION} SOVERSION 1)
target_link_libraries(fastdb_static PUBLIC Threads::Threads)
target_link_libraries(fastdb_shared PRIVATE Threads::Threads $<BUILD_INTERFACE:fastdb_build_flags>)
target_link_options(fastdb_static INTERFACE "$<BUILD_INTERFACE:${fastdb_sanitize_flags}>")

add_executable(fastdb-cli cli/main.c)
target_link_libraries(fastdb-cli PRIVATE fastdb_static fastdb_build_flags)
add_executable(fastdb-legacy-export cli/legacy_export.c)
target_include_directories(fastdb-legacy-export PRIVATE src/storage)
target_link_libraries(fastdb-legacy-export PRIVATE fastdb_static fastdb_build_flags)

install(TARGETS fastdb_static fastdb_shared fastdb-cli fastdb-legacy-export EXPORT FastDBTargets)
install(DIRECTORY include/fastdb DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
set(fastdb_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/FastDB")
install(EXPORT FastDBTargets NAMESPACE FastDB:: DESTINATION "${fastdb_package_dir}")
configure_package_config_file(cmake/FastDBConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/FastDBConfig.cmake" INSTALL_DESTINATION "${fastdb_package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/FastDBConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/FastDBConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/FastDBConfigVersion.cmake"
    DESTINATION "${fastdb_package_dir}")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
