#ifndef _STUB_LINUX_IO_H
#define _STUB_LINUX_IO_H
#include <stdint.h>
static inline uint32_t readl(const volatile void *p) { return *(const volatile uint32_t *)p; }
#endif
