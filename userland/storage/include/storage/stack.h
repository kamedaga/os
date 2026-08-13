#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STORAGE_STACK_ROOT_DEVICE_NAME "rootfs-nvme"

enum {
    STORAGE_STACK_ROOTFS_GPT_PARTITION_INDEX = 2,
    STORAGE_STACK_MODULE_CAPACITY = 8,
    STORAGE_STACK_MODULE_NAME_BYTES = 64,
    STORAGE_STACK_MODULE_FLAG_ALLOW_MISSING_INIT = 1u << 0,
};

typedef enum storage_stack_phase {
    STORAGE_STACK_PHASE_NVME = 1,
    STORAGE_STACK_PHASE_FILESYSTEM = 2,
} storage_stack_phase_t;

typedef enum storage_stack_module_id {
#define STORAGE_STACK_MODULE(module_id, module_phase, name, bootfs_path, init_policy) \
    STORAGE_STACK_MODULE_##module_id,
#include "storage/stack.def"
#undef STORAGE_STACK_MODULE
    STORAGE_STACK_MODULE_COUNT,
} storage_stack_module_id_t;

typedef struct storage_stack_module_spec {
    storage_stack_module_id_t id;
    storage_stack_phase_t phase;
    const char *name;
    const char *bootfs_path;
    uint32_t flags;
} storage_stack_module_spec_t;

#define STORAGE_STACK_INIT_FLAGS_ALLOW_MISSING_INIT \
    STORAGE_STACK_MODULE_FLAG_ALLOW_MISSING_INIT
#define STORAGE_STACK_INIT_FLAGS_REQUIRED_INIT 0u
#define STORAGE_STACK_MODULE(module_id, module_phase, module_name, module_bootfs_path, init_policy) \
    { \
        .id = STORAGE_STACK_MODULE_##module_id, \
        .phase = STORAGE_STACK_PHASE_##module_phase, \
        .name = module_name, \
        .bootfs_path = module_bootfs_path, \
        .flags = STORAGE_STACK_INIT_FLAGS_##init_policy, \
    },
static const storage_stack_module_spec_t storage_stack_modules[] = {
#include "storage/stack.def"
};
#undef STORAGE_STACK_MODULE
#undef STORAGE_STACK_INIT_FLAGS_REQUIRED_INIT
#undef STORAGE_STACK_INIT_FLAGS_ALLOW_MISSING_INIT

_Static_assert(
    sizeof(storage_stack_modules) / sizeof(storage_stack_modules[0]) ==
        STORAGE_STACK_MODULE_COUNT,
    "storage stack manifest count");
_Static_assert(
    (unsigned int)STORAGE_STACK_MODULE_COUNT <= STORAGE_STACK_MODULE_CAPACITY,
    "storage stack exceeds bootstrap capacity");

static inline const storage_stack_module_spec_t *storage_stack_spec(
    storage_stack_module_id_t id)
{
    if ((unsigned int)id >= (unsigned int)STORAGE_STACK_MODULE_COUNT) {
        return NULL;
    }
    return &storage_stack_modules[id];
}

static inline const storage_stack_module_spec_t *storage_stack_find(
    const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < STORAGE_STACK_MODULE_COUNT; ++i) {
        if (strcmp(storage_stack_modules[i].name, name) == 0) {
            return &storage_stack_modules[i];
        }
    }
    return NULL;
}
