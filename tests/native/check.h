#ifndef FASTDB_TEST_CHECK_H
#define FASTDB_TEST_CHECK_H

#include "fastdb/fastdb.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
        abort(); \
    } \
} while (0)

#define STATUS(expression, expected) do { \
    FastDBStatus actual_status = (expression); \
    if (actual_status != (expected)) { \
        fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", \
                __FILE__, __LINE__, #expression, fastdb_status_string(actual_status), \
                fastdb_status_string(expected)); \
        abort(); \
    } \
} while (0)

#define OK(expression) STATUS(expression, FASTDB_OK)

#endif
