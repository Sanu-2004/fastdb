#ifndef FASTDB_GROUP_COMMIT_H
#define FASTDB_GROUP_COMMIT_H

#include "fastdb/fastdb.h"

typedef struct FDBGroupCommit FDBGroupCommit;
struct Operation;

typedef struct FDBGroupedWrite {
    struct FDBGroupedWrite *next;
    const struct Operation *operation;
    FastDBStatus status;
    bool done;
} FDBGroupedWrite;

bool fdb_group_commit_valid(const FastDBGroupCommitOptions *options);
size_t fdb_group_commit_workspace(void);
FastDBStatus fdb_group_commit_start(FastDB *db, const FastDBGroupCommitOptions *options,
                                     FDBGroupCommit **out);
FastDBStatus fdb_group_commit_stop(FDBGroupCommit **group);
size_t fdb_group_commit_pending(FDBGroupCommit *group);
/* The submitter owns the operation and buffers until the result is returned. */
FastDBStatus fdb_group_commit_submit(FDBGroupCommit *group, const struct Operation *operation,
                                      size_t charge);
/* Invoked by the worker without holding the queue mutex. */
void fdb_engine_commit_group(FastDB *db, FDBGroupedWrite *list);

#endif
