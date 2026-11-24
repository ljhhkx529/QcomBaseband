/* The host stub of XNU's sys/errno.h: the adapter only needs ENETDOWN. */
#ifndef STUB_SYS_ERRNO_H
#define STUB_SYS_ERRNO_H
#include <errno.h>
#ifndef ENETDOWN
#define ENETDOWN 100
#endif
#endif
