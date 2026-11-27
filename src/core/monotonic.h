#ifndef FASTDB_MONOTONIC_H
#define FASTDB_MONOTONIC_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

bool fdb_monotonic_deadline(uint32_t milliseconds, struct timespec *deadline);
int fdb_monotonic_condition_init(pthread_cond_t *condition);

#endif
