/* The host tests compile the adapter outside the kernel: only what the console report hook names is here (the
 * process list it walks). The tests never call it. */
#pragma once
#include <sys/types.h>
#include <sys/queue.h>

struct proc;
LIST_HEAD(proclist, proc);

struct proc {
    char p_comm[17];
    pid_t p_pid;
    LIST_ENTRY(proc) p_list;
};

static struct proclist allproc;
static inline void proc_list_lock(void) {}
static inline void proc_list_unlock(void) {}
