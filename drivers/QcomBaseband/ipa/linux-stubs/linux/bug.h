#ifndef _STUB_LINUX_BUG_H
#define _STUB_LINUX_BUG_H
#include <linux/types.h>
#define BUG() do {} while (0)
#define WARN_ON_ONCE(x) do {} while (0)
#define WARN_ONCE(x) do {} while (0)
#define WARN(cond, ...) (0)
#define WARN_ON(cond) (0)
#endif
