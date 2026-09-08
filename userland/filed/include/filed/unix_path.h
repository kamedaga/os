#pragma once
#include <stdint.h>

/* Private channel created by seed0root: only unixd receives its CALL end.
 * This is deliberately not an operation on filed's public endpoint. */
#define FILED_UNIX_PATH_MAGIC UINT64_C(0x3154485058494e55)
enum { FILED_UNIX_PATH_OPEN = 0, FILED_UNIX_PATH_CREATE = 1, FILED_UNIX_PATH_RELEASE = 2 };
struct filed_unix_path {
    uint64_t magic, operation, directory, mode;
    uint64_t hold, filesystem, inode;
    int64_t status;
    char path[109];
};
_Static_assert(sizeof(struct filed_unix_path) == 176, "private UNIX pathname wire");
struct filed_runtime;
int filed_unix_path_receive(struct filed_runtime *runtime);
void filed_unix_path_disconnect(struct filed_runtime *runtime);
