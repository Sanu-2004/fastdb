#include "group_commit.h"
#include "monotonic.h"

#include <errno.h>
#include <stdlib.h>

struct FDBGroupCommit {
    FastDB *db;
    FastDBGroupCommitOptions options;
    pthread_mutex_t mutex;
    pthread_cond_t work;
    pthread_cond_t finished;
    pthread_t thread;
    FDBGroupedWrite *first;
    FDBGroupedWrite **tail;
    size_t pending;
    size_t pending_bytes;
    bool stopping;
};

void fastdb_group_commit_options_default(FastDBGroupCommitOptions *options)
{
    if (options != NULL)
        *options = (FastDBGroupCommitOptions){16, 64 * 1024 * 1024, 1};
}

size_t fdb_group_commit_workspace(void)
{
    return sizeof(FDBGroupCommit);
}

static void collect_writes(FDBGroupCommit *group)
{
    struct timespec deadline;
    uint32_t delay = group->options.collection_delay_ms;
    if (delay == 0 || !fdb_monotonic_deadline(delay, &deadline))
        return;
    while (group->pending < group->options.max_pending_requests &&
           group->pending_bytes < group->options.max_pending_bytes &&
           pthread_cond_timedwait(&group->work, &group->mutex, &deadline) == 0) {
        /* New arrivals share the original deadline. */
    }
}

static void *group_worker(void *arg)
{
    FDBGroupCommit *group = arg;
    pthread_mutex_lock(&group->mutex);
    while (!group->stopping) {
        if (group->first == NULL) {
            pthread_cond_wait(&group->work, &group->mutex);
            continue;
        }
        collect_writes(group);
        FDBGroupedWrite *list = group->first;
        group->first = NULL;
        group->tail = &group->first;
        pthread_mutex_unlock(&group->mutex);
        fdb_engine_commit_group(group->db, list);
        pthread_mutex_lock(&group->mutex);
        for (FDBGroupedWrite *write = list; write != NULL; write = write->next)
            write->done = true;
        pthread_cond_broadcast(&group->finished);
    }
    pthread_mutex_unlock(&group->mutex);
    return NULL;
}

static int start_worker(FDBGroupCommit *group)
{
    int error = pthread_mutex_init(&group->mutex, NULL);
    if (error != 0)
        return error;
    error = fdb_monotonic_condition_init(&group->work);
    if (error == 0) {
        error = pthread_cond_init(&group->finished, NULL);
        if (error == 0) {
            error = pthread_create(&group->thread, NULL, group_worker, group);
            if (error != 0)
                pthread_cond_destroy(&group->finished);
        }
        if (error != 0)
            pthread_cond_destroy(&group->work);
    }
    if (error != 0)
        pthread_mutex_destroy(&group->mutex);
    return error;
}

FastDBStatus fdb_group_commit_start(FastDB *db, const FastDBGroupCommitOptions *options,
                                     FDBGroupCommit **out)
{
    FDBGroupCommit *group = malloc(sizeof(*group));
    if (group == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *group = (FDBGroupCommit){.db = db, .options = *options, .tail = &group->first};
    int error = start_worker(group);
    if (error != 0) {
        free(group);
        return error == ENOMEM ? FASTDB_OUT_OF_MEMORY : FASTDB_IO_ERROR;
    }
    *out = group;
    return FASTDB_OK;
}

FastDBStatus fdb_group_commit_stop(FDBGroupCommit **slot)
{
    FDBGroupCommit *group = *slot;
    if (group == NULL)
        return FASTDB_OK;
    pthread_mutex_lock(&group->mutex);
    if (group->pending != 0) {
        pthread_mutex_unlock(&group->mutex);
        return FASTDB_BUSY;
    }
    group->stopping = true;
    pthread_cond_signal(&group->work);
    pthread_mutex_unlock(&group->mutex);
    if (pthread_join(group->thread, NULL) != 0)
        return FASTDB_BUSY;
    pthread_cond_destroy(&group->finished);
    pthread_cond_destroy(&group->work);
    pthread_mutex_destroy(&group->mutex);
    free(group);
    *slot = NULL;
    return FASTDB_OK;
}

size_t fdb_group_commit_pending(FDBGroupCommit *group)
{
    if (group == NULL)
        return 0;
    pthread_mutex_lock(&group->mutex);
    size_t count = group->pending;
    pthread_mutex_unlock(&group->mutex);
    return count;
}

FastDBStatus fdb_group_commit_submit(FDBGroupCommit *group, const struct Operation *operation,
                                      size_t charge)
{
    if (charge > group->options.max_pending_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    /* A pending cancellation cannot abandon a request borrowed from this stack. */
    int cancellation;
    if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancellation) != 0)
        return FASTDB_IO_ERROR;
    pthread_mutex_lock(&group->mutex);
    FastDBStatus status = FASTDB_BUSY;
    if (group->stopping) {
        status = FASTDB_CLOSED;
    } else if (group->pending < group->options.max_pending_requests &&
               charge <= group->options.max_pending_bytes - group->pending_bytes) {
        FDBGroupedWrite request = {.operation = operation};
        *group->tail = &request;
        group->tail = &request.next;
        ++group->pending;
        group->pending_bytes += charge;
        pthread_cond_signal(&group->work);
        while (!request.done)
            pthread_cond_wait(&group->finished, &group->mutex);
        status = request.status;
        --group->pending;
        group->pending_bytes -= charge;
    }
    pthread_mutex_unlock(&group->mutex);
    pthread_setcancelstate(cancellation, NULL);
    return status;
}
