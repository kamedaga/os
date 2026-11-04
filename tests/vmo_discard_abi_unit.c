#include <stddef.h>
#include "pacha/abi.h"
#include "../musl/pachaos/include/pachaos/abi.h"
_Static_assert(PACHAOS_SYSCALL_VMO_DISCARD == 64, "musl discard number");
_Static_assert(sizeof(struct pachaos_vmo_discard_range) == 16, "musl range size");
_Static_assert(_Alignof(struct pachaos_vmo_discard_range) == 8, "musl range align");
_Static_assert(offsetof(struct pachaos_vmo_discard_range, page_count) == 8, "musl range offset");
_Static_assert(PACHA_FD_SYSCALL_VMO_DISCARD == 64, "discard number");
_Static_assert(PACHA_FD_SYSCALL_VMO_CACHE_POLICY == 65, "cache number");
_Static_assert(sizeof(struct pacha_vmo_discard_range) == 16, "range size");
_Static_assert(_Alignof(struct pacha_vmo_discard_range) == 8, "range alignment");
_Static_assert(offsetof(struct pacha_vmo_discard_range, page_count) == 8, "range offset");
int main(void) { return 0; }
