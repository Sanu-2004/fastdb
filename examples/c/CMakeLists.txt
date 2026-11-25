cmake_minimum_required(VERSION 3.20)
project(FastDBExample LANGUAGES C)

find_package(FastDB 0.1 CONFIG REQUIRED)
add_executable(fastdb-example-static hello.c)
target_link_libraries(fastdb-example-static PRIVATE FastDB::static)
add_executable(fastdb-example-shared hello.c)
target_link_libraries(fastdb-example-shared PRIVATE FastDB::shared)
