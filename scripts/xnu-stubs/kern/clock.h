/* The host stub of XNU's kern/clock.h: the adapter needs the uptime clock only to count milliseconds. */
#ifndef STUB_KERN_CLOCK_H
#define STUB_KERN_CLOCK_H
#include <stdint.h>
#include <time.h>

/* XNU's clock_get_uptime fills the absolute time since the boot, in ticks. The host has no boot clock, so the
   monotonic clock stands in for it. The unit does not matter: the caller only divides the nanoseconds by a
   million to get milliseconds. */
static inline void clock_get_uptime(uint64_t *ticks)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    *ticks = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* XNU's absolutetime_to_nanoseconds converts an absolute time to nanoseconds. The stub's clock is already in
   nanoseconds, so the conversion is the identity. */
static inline void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result)
{
    *result = abstime;
}

#endif
