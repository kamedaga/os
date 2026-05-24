#ifndef CAPABILITYOS_EXEC_ABI_H
#define CAPABILITYOS_EXEC_ABI_H

typedef unsigned char exec_u8;
typedef unsigned short exec_u16;
typedef unsigned long long exec_u64;

#define EXEC_OFFSETOF(type, member) __builtin_offsetof(type, member)

#define EXEC_BOOTSTRAP_MAGIC 0x45584543424F4F54ULL
#define EXEC_BOOTSTRAP_VERSION 3ULL
#define EXEC_BOOTSTRAP_TARGET_VA 0x3C002000ULL
#define EXEC_BOOTSTRAP_FLAG_SERVICE_MODE (1ULL << 0)

#define EXEC_MAX_ARGV 8
#define EXEC_MAX_ENVP 16
#define EXEC_MAX_ARG_DATA_BYTES 2048

#define EXEC_LAUNCH_ENDPOINT_ID 0x93ULL
#define EXEC_LAUNCH_REQUEST_MAGIC 0x4558454353565251ULL
#define EXEC_LAUNCH_RESPONSE_MAGIC 0x4558454353565252ULL
#define EXEC_LAUNCH_VERSION 1ULL
#define EXEC_LAUNCH_OP_START 1ULL
#define EXEC_LAUNCH_OP_START_READY 2ULL
#define EXEC_LAUNCH_OP_STARTED 3ULL

#define EXEC_LAUNCH_STATUS_OK 0ULL
#define EXEC_LAUNCH_STATUS_INVALID 1ULL
#define EXEC_LAUNCH_STATUS_MAP_FAILED 2ULL
#define EXEC_LAUNCH_STATUS_LAUNCH_FAILED 3ULL
#define EXEC_LAUNCH_STATUS_START_FAILED 4ULL

#define EXEC_USER_LAYOUT_LOW_VA 0x20000000ULL
#define EXEC_USER_LAYOUT_TOP_VA 0x800000000000ULL
#define EXEC_USER_DYNAMIC_MAP_BASE_VA 0x23000000ULL
#define EXEC_USER_DYNAMIC_MAP_END_VA 0x3C000000ULL
#define EXEC_USER_ET_DYN_BASE_VA 0x20000000ULL
#define EXEC_USER_STACK_TOP_VA 0x3C000000ULL
#define EXEC_USER_STACK_PAGE_COUNT 128ULL
#define EXEC_LINUX_MMAP_BASE_VA 0x700000000000ULL
#define EXEC_LINUX_BRK_INITIAL_VA 0x3B000000ULL

struct exec_bootstrap_config {
    exec_u64 magic;
    exec_u64 version;
    exec_u64 executable_vm_token;
    exec_u64 executable_file_bytes;
    exec_u64 flags;
    exec_u64 interpreter_vm_token;
    exec_u64 interpreter_file_bytes;
    exec_u64 bootfs_vm_token;
    exec_u64 bootfs_file_bytes;
    exec_u64 fs_endpoint_id;
    exec_u64 fs_compat_process_slot;
    exec_u64 abi_trap_endpoint_id;
    exec_u64 abi_trap_endpoint_process_slot;
    exec_u64 abi_trap_flavor;
    exec_u64 abi_trap_request_page_va;
    exec_u16 execfn_offset;
    exec_u16 execfn_bytes;
    exec_u16 argv_count;
    exec_u16 envp_count;
    exec_u16 arg_data_bytes;
    exec_u16 reserved_arg0;
    exec_u16 argv_offsets[EXEC_MAX_ARGV];
    exec_u16 argv_bytes[EXEC_MAX_ARGV];
    exec_u16 envp_offsets[EXEC_MAX_ENVP];
    exec_u16 envp_bytes[EXEC_MAX_ENVP];
    exec_u8 arg_data[EXEC_MAX_ARG_DATA_BYTES];
    exec_u64 user_low_va;
    exec_u64 user_top_va;
    exec_u64 dynamic_map_base_va;
    exec_u64 dynamic_map_end_va;
    exec_u64 et_dyn_base_va;
    exec_u64 stack_top_va;
    exec_u64 stack_page_count;
    exec_u64 mmap_base_va;
    exec_u64 brk_initial_va;
};

struct exec_launch_request {
    exec_u64 magic;
    exec_u64 version;
    exec_u64 op;
    exec_u64 seq;
    exec_u64 response_token;
    struct exec_bootstrap_config config;
};

struct exec_launch_response {
    exec_u64 magic;
    exec_u64 version;
    exec_u64 op;
    exec_u64 seq;
    exec_u64 status;
    exec_u64 child_process_slot;
};

_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, magic) == 0, "exec cfg magic offset");
_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, executable_vm_token) == 16, "exec cfg executable token offset");
_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, abi_trap_request_page_va) == 112, "exec cfg abi trap request offset");
_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, execfn_offset) == 120, "exec cfg argv header offset");
_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, argv_offsets) == 132, "exec cfg argv offsets offset");
_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, envp_offsets) == 164, "exec cfg envp offsets offset");
_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, arg_data) == 228, "exec cfg arg data offset");
_Static_assert(EXEC_OFFSETOF(struct exec_bootstrap_config, user_low_va) == 2280, "exec cfg layout offset");
_Static_assert(sizeof(struct exec_bootstrap_config) == 2352, "exec cfg size");
_Static_assert(EXEC_OFFSETOF(struct exec_launch_request, config) == 40, "exec request config offset");
_Static_assert(sizeof(struct exec_launch_response) == 48, "exec response size");

#endif
