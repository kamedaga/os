#pragma once

#include <stdint.h>

#include "pacha/service_abi.h"

/* Replies use pacha_service_envelope.status: 0 or a negative Linux errno. */

enum {
    LPRS_SERVICE_ID = PACHA_SERVICE_ID_LPRS,
    LPRS_PAGE_BYTES = PACHA_SERVICE_PAGE_BYTES,
    LPRS_PAYLOAD_BYTES = PACHA_SERVICE_PAGE_BYTES - PACHA_SERVICE_HEADER_BYTES,
    LPRS_CTTY_BYTES = 64u,
    LPRS_CWD_BYTES = 480u,
    LPRS_PROCESS_STATE_SIGCHLD_PENDING = 1ull << 0,
    LPRS_PROCESS_STATE_HAS_CHILDREN = 1ull << 1,
    LPRS_PROCESS_LIST_CAPACITY = (LPRS_PAYLOAD_BYTES - 32u) / 8u,

    LPRS_OP_HELLO = 0u,

    LPRS_OP_PROCESS_REGISTER_EXEC = 1u,
    LPRS_OP_PROCESS_REGISTER_FD = 2u,
    LPRS_OP_PROCESS_GET_STATE = 3u,
    LPRS_OP_PROCESS_LIST = 4u,
    LPRS_OP_PROCESS_FORK_BEGIN = 5u,
    LPRS_OP_PROCESS_FORK_CANCEL = 6u,
    LPRS_OP_PROCESS_FORK_PARENT_REGISTER = 7u,
    LPRS_OP_PROCESS_FORK_CHILD_READY = 8u,
    LPRS_OP_PROCESS_EXEC_COMMIT_BEGIN = 9u,
    LPRS_OP_PROCESS_EXEC_COMMIT_CANCEL = 10u,
    LPRS_OP_PROCESS_EXEC_COMMIT_DONE = 11u,
    LPRS_OP_PROCESS_WAIT4 = 12u,
    LPRS_OP_PROCESS_SETPGID = 13u,
    LPRS_OP_PROCESS_SETSID = 14u,
    LPRS_OP_PROCESS_GETPGID = 15u,
    LPRS_OP_PROCESS_GETSID = 16u,
    LPRS_OP_PROCESS_SET_PDEATHSIG = 17u,
    LPRS_OP_PROCESS_GET_PDEATHSIG = 18u,

    LPRS_OP_SIGNAL_KILL = 19u,
    LPRS_OP_SIGNAL_DELIVER_TTY = 20u,

    LPRS_OP_CWD_GET = 21u,
    LPRS_OP_CWD_SET = 22u,

    LPRS_OP_DIAG_DUMP = 23u,
    LPRS_OP_DIAG_ERROR_GET = 24u,

    /* Process introspection behind /proc/<pid>.  Appended rather than grouped
     * with the process operations above so every existing operation keeps the
     * number already compiled into the personality images. */
    LPRS_OP_PROCESS_SET_COMM = 25u,
    LPRS_OP_PROCESS_QUERY = 26u,
    LPRS_OP_PROCESS_DIAG_ATTACH = 27u,
};

enum {
    LPRS_PROCESS_COMM_BYTES = 32u,
    LPRS_PROCESS_CMDLINE_BYTES = 256u,

    /* What the supervisor alone can tell about a process.  It records
     * lifetime, not scheduling, so a live process is reported as running and
     * only an exited-but-unreaped one is distinguishable. */
    LPRS_PROCESS_RUN_STATE_RUNNING = 0u,
    LPRS_PROCESS_RUN_STATE_ZOMBIE = 1u,

    LPRS_DIAG_SLOT_COUNT = 256u,
    LPRS_DIAG_SLOT_BYTES = 64u,
    LPRS_DIAG_PAGE_BYTES = LPRS_DIAG_SLOT_COUNT * LPRS_DIAG_SLOT_BYTES,
};

/* Not a Linux call number: marks a process that is running its own code
 * rather than sitting inside the personality. */
#define LPRS_DIAG_SYSCALL_NONE 0xffffffffffffffffull

typedef struct lprs_process_set_comm {
    uint64_t token;
    char comm[LPRS_PROCESS_COMM_BYTES];
    char cmdline[LPRS_PROCESS_CMDLINE_BYTES];
} lprs_process_set_comm_t;

/* One cache line per process in the shared diagnostic page.  Only that process
 * writes it, with plain stores and no system call, and only the supervisor
 * reads it.  seq is odd while a write is in flight, so a reader that sees the
 * same even value before and after the body has a consistent sample without
 * either side taking a lock. */
typedef struct lprs_diag_slot {
    uint64_t seq;
    uint64_t syscall_nr;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t enter_tick;
    uint64_t reserved[3];
} lprs_diag_slot_t;

typedef struct lprs_diag_attach {
    uint64_t token;
    uint64_t slot_index;  /* filled by the supervisor */
    uint64_t slot_count;  /* filled by the supervisor */
} lprs_diag_attach_t;

typedef struct lprs_process_query {
    uint64_t token;      /* caller */
    uint64_t pid;        /* target, supplied by the caller */
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgrp;
    uint64_t run_state;
    uint64_t exit_status;
    uint64_t flags;
    /* Sampled from the target's diagnostic slot.  diag_valid is zero when the
     * process never attached or the sample was torn. */
    uint64_t diag_valid;
    uint64_t syscall_nr;
    uint64_t syscall_arg0;
    uint64_t syscall_arg1;
    uint64_t syscall_enter_tick;
    char comm[LPRS_PROCESS_COMM_BYTES];
    char cmdline[LPRS_PROCESS_CMDLINE_BYTES];
    char cwd[LPRS_CWD_BYTES];
} lprs_process_query_t;

typedef struct lprs_token_request {
    uint64_t token;
} lprs_token_request_t;

typedef struct lprs_process_state {
    uint64_t token;
    uint64_t generation;
    uint64_t child_sequence;
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgrp;
    uint64_t foreground_pgrp;
    uint64_t cwd_handle;
    uint64_t flags;
    char ctty[LPRS_CTTY_BYTES];
    char cwd[LPRS_CWD_BYTES];
} lprs_process_state_t;

typedef struct lprs_register_exec {
    lprs_process_state_t state;
} lprs_register_exec_t;

typedef struct lprs_process_list {
    uint64_t token;
    uint64_t offset;
    uint64_t capacity;
    uint64_t count;
    uint64_t pids[LPRS_PROCESS_LIST_CAPACITY];
} lprs_process_list_t;

typedef struct lprs_fork {
    uint64_t parent_token;
    uint64_t child_token;
    uint64_t child_pid;
    uint64_t child_ppid;
    uint64_t child_sid;
    uint64_t child_pgrp;
} lprs_fork_t;

typedef struct lprs_wait4 {
    uint64_t token;
    int64_t requested_pid;
    uint64_t options;
    int64_t result_pid;
    uint64_t status;
    uint64_t exit_code;
} lprs_wait4_t;

#define LPRS_WAIT4_RESULT_PACK(pid, status) \
    ((((uint64_t)(uint32_t)(status)) << 32) | (uint64_t)(uint32_t)(pid))
#define LPRS_WAIT4_RESULT_PID(value) ((uint32_t)((value) & 0xffffffffull))
#define LPRS_WAIT4_RESULT_STATUS(value) ((uint32_t)(((value) >> 32) & 0xffffffffull))

typedef struct lprs_pid_op {
    uint64_t token;
    int64_t pid;
    int64_t value;
    uint64_t result;
} lprs_pid_op_t;

typedef struct lprs_pdeathsig {
    uint64_t token;
    uint64_t signal;
    uint64_t result;
} lprs_pdeathsig_t;

typedef struct lprs_kill {
    uint64_t token;
    int64_t pid;
    uint64_t signal;
    uint64_t delivered;
} lprs_kill_t;

typedef struct lprs_tty_signal {
    uint64_t pgrp;
    uint64_t signal;
    uint64_t delivered;
} lprs_tty_signal_t;

typedef struct lprs_cwd {
    uint64_t token;
    uint64_t cwd_handle;
    char cwd[LPRS_CWD_BYTES];
} lprs_cwd_t;

typedef struct lprs_diag_error_get {
    uint64_t token;
} lprs_diag_error_get_t;

_Static_assert(sizeof(lprs_process_state_t) == 624, "lprs_process_state size");
_Static_assert(sizeof(lprs_process_list_t) == LPRS_PAYLOAD_BYTES,
    "lprs_process_list fills payload");
_Static_assert(sizeof(lprs_fork_t) == 48, "lprs_fork size");
_Static_assert(sizeof(lprs_wait4_t) == 48, "lprs_wait4 size");
_Static_assert(sizeof(lprs_pid_op_t) == 32, "lprs_pid_op size");
_Static_assert(sizeof(lprs_pdeathsig_t) == 24, "lprs_pdeathsig size");
_Static_assert(sizeof(lprs_kill_t) == 32, "lprs_kill size");
_Static_assert(sizeof(lprs_tty_signal_t) == 24, "lprs_tty_signal size");
_Static_assert(sizeof(lprs_cwd_t) == 496, "lprs_cwd size");
_Static_assert(sizeof(lprs_process_state_t) <= LPRS_PAYLOAD_BYTES,
    "lprs_process_state fits payload");
_Static_assert(sizeof(lprs_process_set_comm_t) == 296, "lprs_process_set_comm size");
_Static_assert(sizeof(lprs_process_query_t) == 872, "lprs_process_query size");
_Static_assert(sizeof(lprs_process_query_t) <= LPRS_PAYLOAD_BYTES,
    "lprs_process_query fits payload");
_Static_assert(sizeof(lprs_diag_slot_t) == LPRS_DIAG_SLOT_BYTES,
    "lprs_diag_slot is one cache line");
_Static_assert(sizeof(lprs_diag_attach_t) == 24, "lprs_diag_attach size");
