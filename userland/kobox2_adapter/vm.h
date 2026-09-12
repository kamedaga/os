/* SPDX-License-Identifier: MIT */
#ifndef PACHA_KOBOX_VM_H
#define PACHA_KOBOX_VM_H

#include "mm/host.h"

struct ph_vm_space;

/* Creates a separate native address space; never copies the Linux core into
 * the child. The ELF is a trusted native bootstrap, not the hosted user ELF.
 * Create/destroy require a registered native thread outside Linux execution. */
struct ph_vm_space *ph_vm_create(const void *file, size_t file_size,
    uint64_t start, uint64_t length);
uint64_t ph_vm_pid(const struct ph_vm_space *space);
int ph_vm_terminate(struct ph_vm_space *space);
/* Destroy only after all Linux tasks/bindings stop referring to the handle.
 * close leaves a tombstone; destroy joins the native event producer. */
void ph_vm_destroy(struct ph_vm_space *space);
int ph_vm_probe(void *space, uint64_t address, unsigned write,
    uint64_t value, uint64_t sequence);
int ph_vm_issue_syscall(void *space, uint64_t number,
    const uint64_t arguments[6], uint64_t sequence);
extern const struct kobox_linux_vm_host_operations ph_vm_ops;

/* Boot-only Gate wiring, separate from native transport implementation. */
void ph_vm_gate_prepare(void);
void ph_verify_vm_gate(void);

#endif
