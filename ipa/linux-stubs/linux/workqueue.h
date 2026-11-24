#ifndef _STUB_LINUX_WORKQUEUE_H
#define _STUB_LINUX_WORKQUEUE_H
#include <linux/types.h>
struct work_struct { int dummy; };
struct delayed_work { struct work_struct work; };
#endif
