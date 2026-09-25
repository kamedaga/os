/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_VM_CLIENT_CONTROL_H
#define PACHA_KOBOX_VM_CLIENT_CONTROL_H

#include <stdint.h>
#include <stdatomic.h>
#include "vm_layout.h"

/* Private native test-client bootstrap, not a controller or LPR wire ABI.
 * No core pointer or RAM descriptor crosses this boundary. Addresses below
 * refer only to the child. The manager keeps authoritative bounds privately. */
#define PH_VM_CLIENT_KICK_FD 16
#define PH_VM_CLIENT_EVENT_FD 17

enum ph_vm_client_command {
    PH_VM_CLIENT_ACCESS = 1,
    PH_VM_CLIENT_RESUME,
    PH_VM_CLIENT_PROTECT,
    PH_VM_CLIENT_SYSCALL,
    PH_VM_CLIENT_PARK,
};

enum ph_vm_client_event {
    PH_VM_CLIENT_READY = 1,
    PH_VM_CLIENT_DONE,
    PH_VM_CLIENT_FAULT,
    PH_VM_CLIENT_SYSCALL_ENTRY,
    PH_VM_CLIENT_PARKED,
};

/* Named machine registers, not pt_regs or an LPR runtime-stack layout. */
struct ph_vm_client_registers {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rsp, rflags;
    uint64_t fs_base, gs_base;
};

struct ph_vm_client_control {
    _Atomic uint64_t command_sequence;
    uint64_t command;
    uint64_t address;
    uint64_t value;
    uint64_t write;
    _Atomic uint64_t event_sequence;
    uint64_t event;
    uint64_t result;
    uint64_t pid;
    uint64_t fault_address;
    uint64_t fault_ip;
    uint64_t fault_sp;
    uint64_t fault_flags;
    uint64_t fault_error;
    uint64_t fault_vector;
    uint64_t syscall_sequence;
    uint64_t restore_fault;
    uint64_t autonomous;
    uint64_t linux_fs_base;
    uint64_t linux_gs_base;
    uint64_t syscall_entry;
    struct ph_vm_client_registers registers;
    _Alignas(16) unsigned char fp[512];
};

_Static_assert(sizeof(struct ph_vm_client_control) <= 4096, "client control fits one page");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "native client sequence must be lock-free");

#endif
