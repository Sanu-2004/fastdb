#ifndef FASTDB_COMPACTION_H
#define FASTDB_COMPACTION_H

#include "compaction_plan.h"
#include "merge.h"
#include "wal_set.h"

/* The engine lock must exclude changes to the catalog through plan publication. */
FastDBStatus fdb_compact_once(FDBWalSet *set, const FDBCompactionPolicy *policy,
                               const FDBCompactionPlan *plan, const size_t *inputs,
                               const FDBMergeLimits *limits);

#endif
