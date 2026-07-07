#ifndef LPR_SUPERVISOR_IPC_PROTOCOL_H
#define LPR_SUPERVISOR_IPC_PROTOCOL_H

#include <stdint.h>

#define LPRS_WIRE_REQUEST_MAGIC 0x31535152504c524cull
#define LPRS_WIRE_REPLY_MAGIC 0x31595052504c524cull
#define LPRS_WIRE_VERSION 1ull
#define LPRS_WIRE_PAGE_BYTES 8192ull
#define LPRS_WIRE_CTTY_BYTES 64u
#define LPRS_WIRE_CWD_BYTES 480u

enum lprs_wire_op {
    LPRS_WIRE_OP_HELLO = 1,
    LPRS_WIRE_OP_REGISTER_EXEC = 2,
    LPRS_WIRE_OP_REGISTER_PROCESS_FD = 3,
    LPRS_WIRE_OP_GET_PROCESS_STATE = 4,
    LPRS_WIRE_OP_FORK_BEGIN = 5,
    LPRS_WIRE_OP_FORK_PARENT_REGISTER = 6,
    LPRS_WIRE_OP_FORK_CHILD_READY = 7,
    LPRS_WIRE_OP_EXEC_COMMIT_BEGIN = 8,
    LPRS_WIRE_OP_EXEC_COMMIT_DONE = 9,
    LPRS_WIRE_OP_WAIT4 = 10,
    LPRS_WIRE_OP_SETPGID = 11,
    LPRS_WIRE_OP_SETSID = 12,
    LPRS_WIRE_OP_GETPGID = 13,
    LPRS_WIRE_OP_GETSID = 14,
    LPRS_WIRE_OP_KILL = 15,
    LPRS_WIRE_OP_CWD_GET = 16,
    LPRS_WIRE_OP_CWD_SET = 17,
    LPRS_WIRE_OP_FD_TABLE_REPLACE_BEGIN = 18,
    LPRS_WIRE_OP_FD_TABLE_REPLACE_CHUNK = 19,
    LPRS_WIRE_OP_FD_TABLE_REPLACE_COMMIT = 20,
    LPRS_WIRE_OP_FD_TABLE_GET_CHUNK = 21,
    LPRS_WIRE_OP_DELIVER_TTY_SIGNAL = 22,
    LPRS_WIRE_OP_ERROR_GET = 23,
};

enum {
    LPRS_FD_KIND_NONE = 0,
    LPRS_FD_KIND_FILED = 1,
    LPRS_FD_KIND_TTY = 2,
    LPRS_FD_KIND_EVENT = 3,
};

typedef struct lprs_wire_process_state {
    uint64_t token;
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgrp;
    uint64_t foreground_pgrp;
    uint64_t cwd_handle;
    uint64_t flags;
    char ctty[LPRS_WIRE_CTTY_BYTES];
    char cwd[LPRS_WIRE_CWD_BYTES];
} lprs_wire_process_state_t;

typedef struct lprs_wire_register_exec {
    lprs_wire_process_state_t state;
} lprs_wire_register_exec_t;

typedef struct lprs_wire_fork {
    uint64_t parent_token;
    uint64_t child_token;
    uint64_t child_pid;
    uint64_t child_ppid;
    uint64_t child_sid;
    uint64_t child_pgrp;
} lprs_wire_fork_t;

typedef struct lprs_wire_wait4 {
    uint64_t token;
    int64_t requested_pid;
    uint64_t options;
    int64_t result_pid;
    uint64_t status;
    uint64_t exit_code;
} lprs_wire_wait4_t;

#define LPRS_WIRE_WAIT4_RESULT_PACK(pid, status) \
    ((((uint64_t)(uint32_t)(status)) << 32) | (uint64_t)(uint32_t)(pid))
#define LPRS_WIRE_WAIT4_RESULT_PID(value) ((uint32_t)((value) & 0xffffffffull))
#define LPRS_WIRE_WAIT4_RESULT_STATUS(value) ((uint32_t)(((value) >> 32) & 0xffffffffull))

typedef struct lprs_wire_pid_op {
    uint64_t token;
    int64_t pid;
    int64_t value;
    uint64_t result;
} lprs_wire_pid_op_t;

typedef struct lprs_wire_kill {
    uint64_t token;
    int64_t pid;
    uint64_t signal;
    uint64_t delivered;
} lprs_wire_kill_t;

typedef struct lprs_wire_cwd {
    uint64_t token;
    uint64_t cwd_handle;
    char cwd[LPRS_WIRE_CWD_BYTES];
} lprs_wire_cwd_t;

typedef struct lprs_wire_fd_desc {
    uint64_t fd;
    uint64_t kind;
    uint64_t flags;
    uint64_t handle;
    uint64_t offset_or_counter;
} lprs_wire_fd_desc_t;

typedef struct lprs_wire_fd_table_page {
    uint64_t token;
    uint64_t start_index;
    uint64_t total_count;
    uint64_t count;
    lprs_wire_fd_desc_t entries[];
} lprs_wire_fd_table_page_t;

#define LPRS_WIRE_FD_TABLE_PAGE_MAX \
    ((LPRS_WIRE_PAGE_BYTES - sizeof(lprs_wire_fd_table_page_t)) / sizeof(lprs_wire_fd_desc_t))

typedef char lprs_wire_process_state_fits_page[
    sizeof(lprs_wire_process_state_t) <= LPRS_WIRE_PAGE_BYTES ? 1 : -1];
typedef char lprs_wire_fd_table_header_fits_page[
    sizeof(lprs_wire_fd_table_page_t) <= LPRS_WIRE_PAGE_BYTES ? 1 : -1];

#endif
