#include "monotonic.h"

bool fdb_monotonic_deadline(uint32_t milliseconds, struct timespec *deadline)
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

int fdb_monotonic_condition_init(pthread_cond_t *condition)
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
