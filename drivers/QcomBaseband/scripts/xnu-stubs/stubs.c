/* Link stubs for the host tests: the adapter names these, the kernel provides them on device. */
#include <stdint.h>
#include "fuxi_ipa.h"

int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg)
{
    (void)arg_string; (void)arg_ptr; (void)max_arg;
    return 0;
}

int fuxi_ipa_map(struct fuxi_ipa_windows *ipa)
{
    (void)ipa;
    return 1;
}

void fuxi_rmtfs_flush(void)
{
}
