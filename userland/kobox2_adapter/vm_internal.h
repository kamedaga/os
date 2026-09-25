/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_VM_INTERNAL_H
#define PACHA_KOBOX_VM_INTERNAL_H

#include "host.h"
#include "vm.h"
#include "vm_client_control.h"

struct ph_vm_space {
    /* Masks native notifications before locking; monitor never enters Linux
     * while holding this lock. Leaf map/reset need no client execution. */
    atomic_uint lock;
    /* 0: resources owned, 1: one closer is draining, 2: native resources gone. */
    atomic_uint cleanup_state;
    struct ph_vm_memory *memory;
    struct ph_vm_space *memory_next;
    /* All cloned handles are retained under the original launcher until
     * Linux has dropped every binding; close alone leaves a tombstone. */
    struct ph_vm_space *owner, *owned_next, *owned_children;
    const void *bootstrap_file;
    size_t bootstrap_size;
    const struct ph_exec_image *exec_image;
    int process_fd;
    int thread_fd;
    int kick_fd;
    int event_fd;
    int control_fd;
    /* Parent-only capabilities: target max_prot must not exceed Linux PTE
     * permissions even if the child directly invokes native MPROTECT. */
    int ram_grant_fds[8];
    struct ph_vm_client_control *control;
    struct ph_task *monitor;
    uint64_t pid;
    uint64_t start;
    uint64_t length;
    uint64_t sequence;
    uint64_t bootstrap_sequence;
    bool running;
    bool reaped;
    bool exit_delivered;
    bool pending;
    bool syscalls_enabled;
    bool syscall_stopped;
    bool fault_stopped;
    bool syscall_returned;
    bool syscall_in_fault;
    bool native_stopped;
    bool user_started;
    uint64_t syscall_sequence;
    struct kobox_x86_user_regs captured_registers;
    struct kobox_x86_fp_state captured_fp;
    uint32_t fp_mxcsr_mask;
    struct pacha_thread_context initial_context;
    struct kobox_linux_vm_event event;
};

/* Native launcher helpers. Each function validates its own input arithmetic. */
void ph_vm_memory_create(struct ph_vm_space *space);
void ph_vm_memory_destroy(struct ph_vm_space *space);
int ph_vm_memory_share(struct ph_vm_space *child, struct ph_vm_space *parent);
uint64_t ph_vm_lock_memory(struct ph_vm_space *space);
void ph_vm_unlock_memory(struct ph_vm_space *space, uint64_t mask);
int ph_vm_map_pages(void *space, uint64_t address, uint64_t physical,
    size_t length, unsigned protection);
int ph_vm_reset_pages(void *space, uint64_t address, size_t length);
/* Caller holds memory then space locks, like the native event producer. */
uint64_t ph_vm_guest_fault_error(const struct ph_vm_space *space);
bool ph_vm_read_executable(const struct ph_vm_space *space, uint64_t address,
    void *bytes, size_t length);
bool ph_exec_capture_syscall_fault(struct ph_vm_space *space);
uint64_t ph_vm_load_bootstrap(int process_fd, const void *file, size_t file_size);
void ph_vm_map_anonymous(int process_fd, uint64_t address, size_t length);
int ph_vm_native_error(long status);
void ph_vm_event_signal(int fd);
void ph_vm_event_consume(int fd);

/* Caller holds the space lock for idle validation and command publication. */
uint64_t ph_vm_lock_space(struct ph_vm_space *space);
void ph_vm_unlock_space(struct ph_vm_space *space, uint64_t mask);
int ph_vm_idle_status(const struct ph_vm_space *space, uint64_t sequence);
void ph_vm_publish_command(struct ph_vm_space *space, enum ph_vm_client_command command);
void ph_vm_capture_syscall(struct ph_vm_space *space);
void ph_vm_capture_fault(struct ph_vm_space *space);
void ph_vm_park_bootstrap(struct ph_vm_space *space);
void ph_vm_install_lpr_entry(struct ph_vm_space *space);
int ph_vm_start(void *space, const struct kobox_x86_user_regs *registers,
    const struct kobox_x86_fp_state *fp);
int ph_vm_install_user_context(struct ph_vm_space *space,
    const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp);
/* Pause only actual Linux user execution. A racing event stays with monitor. */
int ph_vm_capture_running(struct ph_vm_space *space);
int ph_vm_clone(void *space, uint64_t sequence, uint64_t syscall_sequence,
    bool share_mm, void **child, struct kobox_x86_fp_state *fp);
bool ph_vm_valid_registers(const struct kobox_x86_user_regs *registers);
bool ph_vm_valid_fp(const struct ph_vm_space *space, const struct kobox_x86_fp_state *fp);
struct kobox_x86_user_regs ph_vm_import_registers(const struct ph_vm_client_registers *in);
struct ph_vm_client_registers ph_vm_export_registers(const struct kobox_x86_user_regs *in);
int ph_vm_captured_status(const struct ph_vm_space *space, uint64_t sequence);
int ph_vm_enable_syscalls(void *space);
int ph_vm_snapshot(void *space, uint64_t sequence,
    struct kobox_x86_user_regs *registers, struct kobox_x86_fp_state *fp);
int ph_vm_write_fpregs(void *space, uint64_t sequence, const struct kobox_x86_fp_state *fp);
int ph_vm_restore(void *space, uint64_t sequence,
    const struct kobox_x86_user_regs *registers, const struct kobox_x86_fp_state *fp);
int ph_vm_syscall_return(void *space, uint64_t sequence, uint64_t syscall_sequence,
    const struct kobox_x86_user_regs *registers);

#endif
