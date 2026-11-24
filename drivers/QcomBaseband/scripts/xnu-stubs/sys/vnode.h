#pragma once
#include <sys/types.h>
#include <sys/uio.h>

typedef struct vnode *vnode_t;
typedef struct vfs_context *vfs_context_t;
typedef int errno_t;
#define NULLVP ((vnode_t)0)
typedef struct vnode_attr { unsigned int va_data_size; } vnode_attr;
#define VATTR_INIT(a) do { (void)(a); } while (0)
#define VATTR_WANTED(a, f) do { (void)(a); } while (0)
#define VATTR_IS_SUPPORTED(a, f) 0
#define VNODE_LOOKUP_NOFOLLOW 0

static inline errno_t vnode_open(const char *p, int f, int c, int fl, vnode_t *v, vfs_context_t ctx) { (void)p;(void)f;(void)c;(void)fl;(void)v;(void)ctx; return 2; }
static inline int vnode_close(vnode_t v, int f, vfs_context_t ctx) { (void)v;(void)f;(void)ctx; return 0; }
static inline int vn_rdwr(int op, vnode_t v, void *b, int l, long long o, int s, int io, void *c, int *r, void *p) { (void)op;(void)v;(void)b;(void)l;(void)o;(void)s;(void)io;(void)c;(void)r;(void)p; return 2; }
static inline int vnode_getattr(vnode_t v, vnode_attr *a, vfs_context_t c) { (void)v;(void)a;(void)c; return 2; }
static inline int vnode_lookup(const char *p, int f, vnode_t *v, vfs_context_t c) { (void)p;(void)f;(void)v;(void)c; return 2; }
static inline void vnode_put(vnode_t v) { (void)v; }
static inline vfs_context_t vfs_context_current(void) { return 0; }
static inline vfs_context_t vfs_context_kernel(void) { return 0; }
static inline void *vfs_context_ucred(vfs_context_t c) { (void)c; return 0; }
