#ifndef _STUB_LINUX_KERNEL_H
#define _STUB_LINUX_KERNEL_H
#include <stddef.h>
#include <linux/types.h>
#include <linux/bits.h>
#include <linux/netdevice.h>
#define DECLARE_BITMAP(name, bits) unsigned long name[((bits) + (sizeof(unsigned long) * 8) - 1) / (sizeof(unsigned long) * 8)]
#endif
