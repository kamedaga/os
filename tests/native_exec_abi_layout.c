#include <stddef.h>
#include <asm/unistd.h>
#include "../musl/upstream/arch/pachaos/syscall_arch.h"
#include "filed/payload.h"

_Static_assert(PACHAOS_FILED_OP_EXEC_PATH == FILED_OP_EXEC_PATH, "native exec operation");
_Static_assert(sizeof(struct __pachaos_filed_exec_path) == sizeof(filed_exec_path_t),
    "native exec payload size");
#define CHECK_FIELD(field) _Static_assert(offsetof(struct __pachaos_filed_exec_path, field) == \
    offsetof(filed_exec_path_t, field), "native exec field: " #field)
CHECK_FIELD(inherit_handles);
CHECK_FIELD(fd_grants);
CHECK_FIELD(fd_patches);
CHECK_FIELD(path);
CHECK_FIELD(argv);
CHECK_FIELD(envp);
CHECK_FIELD(strings);
int main(void) { return 0; }
