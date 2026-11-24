/* The host tests compile the adapter outside the kernel: the device-tree lookups the report hooks name, never
 * called by the tests. */
#pragma once

typedef void *DTEntry;

#define kSuccess 0

static inline int SecureDTLookupEntry(const DTEntry searchBase, const char *path, DTEntry *entry)
{
    (void)searchBase; (void)path; (void)entry;
    return -1;
}

static inline int SecureDTGetProperty(const DTEntry entry, const char *propertyName, const void **valueP, unsigned int *sizeP)
{
    (void)entry; (void)propertyName; (void)valueP; (void)sizeP;
    return -1;
}
