#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

enum {
    LPRS_V2_SERVICE_ID = PACHA_SERVICE_ID_LPRS,
    LPRS_V2_PAGE_BYTES = PACHA_SERVICE_PAGE_BYTES,
    LPRS_V2_PAYLOAD_BYTES = PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES,
    LPRS_V2_CTTY_BYTES = 64u,
    LPRS_V2_CWD_BYTES = 480u,

    LPRS_V2_OP_HELLO = 0x0001u,

    LPRS_V2_OP_PROCESS_REGISTER_EXEC = 0x0101u,
    LPRS_V2_OP_PROCESS_REGISTER_FD = 0x0102u,
    LPRS_V2_OP_PROCESS_GET_STATE = 0x0103u,
    LPRS_V2_OP_PROCESS_FORK_BEGIN = 0x0104u,
    LPRS_V2_OP_PROCESS_FORK_PARENT_REGISTER = 0x0105u,
    LPRS_V2_OP_PROCESS_FORK_CHILD_READY = 0x0106u,
    LPRS_V2_OP_PROCESS_EXEC_COMMIT_BEGIN = 0x0107u,
    LPRS_V2_OP_PROCESS_EXEC_COMMIT_DONE = 0x0108u,
    LPRS_V2_OP_PROCESS_WAIT4 = 0x0109u,
    LPRS_V2_OP_PROCESS_SETPGID = 0x010au,
    LPRS_V2_OP_PROCESS_SETSID = 0x010bu,
    LPRS_V2_OP_PROCESS_GETPGID = 0x010cu,
    LPRS_V2_OP_PROCESS_GETSID = 0x010du,

    LPRS_V2_OP_SIGNAL_KILL = 0x0201u,
    LPRS_V2_OP_SIGNAL_DELIVER_TTY = 0x0202u,

    LPRS_V2_OP_FD_TABLE_REPLACE_BEGIN = 0x0301u,
    LPRS_V2_OP_FD_TABLE_REPLACE_CHUNK = 0x0302u,
    LPRS_V2_OP_FD_TABLE_REPLACE_COMMIT = 0x0303u,
    LPRS_V2_OP_FD_TABLE_GET_CHUNK = 0x0304u,

    LPRS_V2_OP_CWD_GET = 0x0401u,
    LPRS_V2_OP_CWD_SET = 0x0402u,

    LPRS_V2_OP_DIAG_DUMP = 0x7f01u,
    LPRS_V2_OP_DIAG_ERROR_GET = 0x7f02u,

    LPRS_V2_FD_KIND_NONE = 0u,
    LPRS_V2_FD_KIND_FILED = 1u,
    LPRS_V2_FD_KIND_TTY = 2u,
    LPRS_V2_FD_KIND_PIPE = 3u,
    LPRS_V2_FD_KIND_EVENT = 4u,
    LPRS_V2_FD_KIND_SOCKET = 5u,
    LPRS_V2_FD_KIND_NATIVE = 6u,

    LPRS_V2_FD_CLOEXEC = 1u << 0,
    LPRS_V2_FILE_NONBLOCK = 1u << 0,
    LPRS_V2_FILE_APPEND = 1u << 1,
};

typedef struct lprs_v2_token_request {
    uint64_t token;
} lprs_v2_token_request_t;

typedef struct lprs_v2_process_state {
    uint64_t token;
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgrp;
    uint64_t foreground_pgrp;
    uint64_t cwd_handle;
    uint64_t flags;
    char ctty[LPRS_V2_CTTY_BYTES];
    char cwd[LPRS_V2_CWD_BYTES];
} lprs_v2_process_state_t;

typedef struct lprs_v2_register_exec {
    lprs_v2_process_state_t state;
} lprs_v2_register_exec_t;

typedef struct lprs_v2_fork {
    uint64_t parent_token;
    uint64_t child_token;
    uint64_t child_pid;
    uint64_t child_ppid;
    uint64_t child_sid;
    uint64_t child_pgrp;
} lprs_v2_fork_t;

typedef struct lprs_v2_wait4 {
    uint64_t token;
    int64_t requested_pid;
    uint64_t options;
    int64_t result_pid;
    uint64_t status;
    uint64_t exit_code;
} lprs_v2_wait4_t;

#define LPRS_V2_WAIT4_RESULT_PACK(pid, status) \
    ((((uint64_t)(uint32_t)(status)) << 32) | (uint64_t)(uint32_t)(pid))
#define LPRS_V2_WAIT4_RESULT_PID(value) ((uint32_t)((value) & 0xffffffffull))
#define LPRS_V2_WAIT4_RESULT_STATUS(value) ((uint32_t)(((value) >> 32) & 0xffffffffull))

typedef struct lprs_v2_pid_op {
    uint64_t token;
    int64_t pid;
    int64_t value;
    uint64_t result;
} lprs_v2_pid_op_t;

typedef struct lprs_v2_kill {
    uint64_t token;
    int64_t pid;
    uint64_t signal;
    uint64_t delivered;
} lprs_v2_kill_t;

typedef struct lprs_v2_tty_signal {
    uint64_t pgrp;
    uint64_t signal;
    uint64_t delivered;
} lprs_v2_tty_signal_t;

typedef struct lprs_v2_cwd {
    uint64_t token;
    uint64_t cwd_handle;
    char cwd[LPRS_V2_CWD_BYTES];
} lprs_v2_cwd_t;

typedef struct lprs_v2_fd_desc {
    uint64_t fd;
    uint64_t kind;
    uint64_t fd_flags;
    uint64_t status_flags;
    uint64_t handle;
    uint64_t offset_or_counter;
} lprs_v2_fd_desc_t;

typedef struct lprs_v2_fd_table_page {
    uint64_t token;
    uint64_t start_index;
    uint64_t total_count;
    uint64_t count;
    lprs_v2_fd_desc_t entries[];
} lprs_v2_fd_table_page_t;

#define LPRS_V2_FD_TABLE_PAGE_MAX \
    ((LPRS_V2_PAYLOAD_BYTES - sizeof(lprs_v2_fd_table_page_t)) / sizeof(lprs_v2_fd_desc_t))

typedef struct lprs_v2_diag_error_get {
    uint64_t token;
} lprs_v2_diag_error_get_t;

_Static_assert(sizeof(lprs_v2_process_state_t) == 608, "lprs_v2_process_state size");
_Static_assert(sizeof(lprs_v2_fork_t) == 48, "lprs_v2_fork size");
_Static_assert(sizeof(lprs_v2_wait4_t) == 48, "lprs_v2_wait4 size");
_Static_assert(sizeof(lprs_v2_pid_op_t) == 32, "lprs_v2_pid_op size");
_Static_assert(sizeof(lprs_v2_kill_t) == 32, "lprs_v2_kill size");
_Static_assert(sizeof(lprs_v2_tty_signal_t) == 24, "lprs_v2_tty_signal size");
_Static_assert(sizeof(lprs_v2_cwd_t) == 496, "lprs_v2_cwd size");
_Static_assert(sizeof(lprs_v2_fd_desc_t) == 48, "lprs_v2_fd_desc size");
_Static_assert(sizeof(lprs_v2_process_state_t) <= LPRS_V2_PAYLOAD_BYTES,
    "lprs_v2_process_state fits payload");
_Static_assert(sizeof(lprs_v2_fd_table_page_t) <= LPRS_V2_PAYLOAD_BYTES,
    "lprs_v2_fd_table_page fits payload");
