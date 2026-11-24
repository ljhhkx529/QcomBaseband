#ifndef _STUB_LINUX_LOG2_H
#define _STUB_LINUX_LOG2_H
#include <linux/types.h>
#define ilog2(x) (sizeof(unsigned int) * 8 - __builtin_clz((unsigned int)(x)) - 1)
#endif
