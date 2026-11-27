#ifndef FASTDB_INTERVAL_SYNC_H
#define FASTDB_INTERVAL_SYNC_H

#include "wal_set.h"

#include <pthread.h>

typedef struct FDBIntervalSync FDBIntervalSync;

size_t fdb_interval_sync_workspace(void);
FastDBStatus fdb_interval_sync_start(FDBWalSet *wal, pthread_mutex_t *mutex, uint32_t milliseconds,
                                     FDBIntervalSync **out);
/* Notify under the engine mutex after publishing an appended frame. */
void fdb_interval_sync_notify(FDBIntervalSync *worker);
/* Stop and join without holding the engine mutex. */
FastDBStatus fdb_interval_sync_stop(FDBIntervalSync **worker);

#endif
