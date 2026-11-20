#ifndef _STUB_LINUX_BITS_H
#define _STUB_LINUX_BITS_H
#include <linux/types.h>
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (64 - 1 - (h))))
#define DECLARE_BITMAP(name, bits) unsigned long name[((bits) + (sizeof(unsigned long) * 8) - 1) / (sizeof(unsigned long) * 8)]
static inline bool is_power_of_2(unsigned long n) { return n && !(n & (n - 1)); }
static inline int __ffs(unsigned int word) { return word ? __builtin_ctz(word) : 0; }
static inline int __fls(unsigned int word) { return word ? 31 - __builtin_clz(word) : -1; }
#endif
