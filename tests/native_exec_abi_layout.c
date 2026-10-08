#include <stddef.h>
#include "kobox2/filesystem.h"
#include "filed/payload.h"

/* Native libc links the shared codec; it has no independent wire mirror. */
_Static_assert(KB2_FILESYSTEM_EXEC_PAYLOAD_SIZE == 128, "exec header");
_Static_assert(sizeof(filed_exec_string_ref_t) == KB2_FILESYSTEM_EXEC_STRING_SIZE, "string span");
_Static_assert(sizeof(filed_exec_fd_grant_t) == KB2_FILESYSTEM_EXEC_GRANT_SIZE, "grant fields");
_Static_assert(sizeof(filed_exec_fd_patch_t) == KB2_FILESYSTEM_EXEC_PATCH_SIZE, "patch fields");
_Static_assert(KB2_FILESYSTEM_OP_EXEC_SELF == KB2_FILESYSTEM_OP_EXEC_PATH + 1, "exec grouping");
int main(void) { return 0; }
