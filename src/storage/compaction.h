#ifndef FASTDB_COMPACTION_H
#define FASTDB_COMPACTION_H

#include "compaction_plan.h"
#include "merge.h"
#include "wal_set.h"
#include <pthread.h>

/* Caller excludes other maintenance. Optional mutex is released only during the merge. */
FastDBStatus fdb_compact_once(FDBWalSet *set, const FDBCompactionPolicy *policy,
                               const FDBCompactionPlan *plan, const uint64_t *input_ids,
                               const FDBMergeLimits *limits, pthread_mutex_t *mutex);

#endif
