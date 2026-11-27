#include "interval_sync.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

struct FDBIntervalSync {
    FDBWalSet *wal;
    pthread_mutex_t *mutex;
    pthread_cond_t wake;
    pthread_t thread;
    uint32_t milliseconds;
    bool stopping;
};

size_t fdb_interval_sync_workspace(void)
{
    return sizeof(FDBIntervalSync);
}

static bool monotonic_deadline(uint32_t milliseconds, struct timespec *deadline)
{
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0 || deadline->tv_sec < 0)
        return false;
    const long billion = 1000000000L;
    long nanoseconds = deadline->tv_nsec + (long)(milliseconds % 1000U) * 1000000L;
    uint64_t seconds = (uint64_t)(milliseconds / 1000U) + (uint64_t)(nanoseconds / billion);
    if (seconds > INT64_MAX - (uint64_t)deadline->tv_sec)
        return false;
    seconds += (uint64_t)deadline->tv_sec;
    time_t converted = (time_t)seconds;
    if (converted < 0 || (uint64_t)converted != seconds)
        return false;
    *deadline = (struct timespec){.tv_sec = converted, .tv_nsec = nanoseconds % billion};
    return true;
}

static bool wait_for_deadline(FDBIntervalSync *worker)
{
    struct timespec deadline;
    if (!monotonic_deadline(worker->milliseconds, &deadline)) {
        worker->wal->poisoned = true;
        return false;
    }
    int result = 0;
    while (!worker->stopping && !worker->wal->poisoned && result == 0)
        result = pthread_cond_timedwait(&worker->wake, worker->mutex, &deadline);
    if (result != 0 && result != ETIMEDOUT)
        worker->wal->poisoned = true;
    return result == ETIMEDOUT && !worker->stopping && !worker->wal->poisoned;
}

static void *sync_worker(void *arg)
{
    FDBIntervalSync *worker = arg;
    pthread_mutex_lock(worker->mutex);
    while (!worker->stopping && !worker->wal->poisoned) {
        if (!worker->wal->wal.dirty) {
            if (pthread_cond_wait(&worker->wake, worker->mutex) != 0)
                worker->wal->poisoned = true;
        } else if (wait_for_deadline(worker) && worker->wal->wal.dirty) {
            (void)fdb_wal_set_sync(worker->wal);
        }
    }
    pthread_mutex_unlock(worker->mutex);
    return NULL;
}

static int init_monotonic_condition(pthread_cond_t *condition)
{
    pthread_condattr_t attributes;
    int error = pthread_condattr_init(&attributes);
    if (error != 0)
        return error;
    error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0)
        error = pthread_cond_init(condition, &attributes);
    pthread_condattr_destroy(&attributes);
    return error;
}

FastDBStatus fdb_interval_sync_start(FDBWalSet *wal, pthread_mutex_t *mutex, uint32_t milliseconds,
                                     FDBIntervalSync **out)
{
    FDBIntervalSync *worker = malloc(sizeof(*worker));
    if (worker == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *worker = (FDBIntervalSync){.wal = wal, .mutex = mutex, .milliseconds = milliseconds};
    int error = init_monotonic_condition(&worker->wake);
    if (error == 0) {
        error = pthread_create(&worker->thread, NULL, sync_worker, worker);
        if (error != 0)
            pthread_cond_destroy(&worker->wake);
    }
    if (error != 0) {
        free(worker);
        return error == ENOMEM ? FASTDB_OUT_OF_MEMORY : FASTDB_IO_ERROR;
    }
    *out = worker;
    return FASTDB_OK;
}

void fdb_interval_sync_notify(FDBIntervalSync *worker)
{
    if (worker != NULL)
        pthread_cond_signal(&worker->wake);
}

FastDBStatus fdb_interval_sync_stop(FDBIntervalSync **slot)
{
    FDBIntervalSync *worker = *slot;
    if (worker == NULL)
        return FASTDB_OK;
    pthread_mutex_lock(worker->mutex);
    worker->stopping = true;
    pthread_cond_signal(&worker->wake);
    pthread_mutex_unlock(worker->mutex);
    if (pthread_join(worker->thread, NULL) != 0)
        return FASTDB_BUSY;
    pthread_cond_destroy(&worker->wake);
    free(worker);
    *slot = NULL;
    return FASTDB_OK;
}
