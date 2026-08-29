#pragma once

/* Component-name storage is shared by filed's wire payloads and its internal
 * vnode cache.  Keep one definition so a wire-valid name cannot be truncated
 * or rejected after the backend has already created it. */
enum {
    FILED_NAME_BYTES = 96u,
};
