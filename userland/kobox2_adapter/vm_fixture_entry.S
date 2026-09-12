/* SPDX-License-Identifier: GPL-2.0-only */
/* Test entries adapted from the common x86 Linux client. The LPR jump does
 * not push a native return address onto a Linux-created clone stack. */
#include <asm/unistd.h>
.text
.globl ph_vm_fixture_fork
.type ph_vm_fixture_fork, @function
ph_vm_fixture_fork:
	pushq %r12
	movq %rsp, ph_vm_fixture_parent_stack(%rip)
	movq %rdi, %r12
	/* Only Linux-owned memory is inherited by Linux fork. Keep the child
	 * off the parent's native dispatcher/bootstrap stack from the outset.
	 */
	leaq 16384(%rdi), %rsp
	andq $-16, %rsp
	xorl %edi, %edi
	xorl %esi, %esi
	xorl %edx, %edx
	xorl %r10d, %r10d
	xorl %r8d, %r8d
	xorl %r9d, %r9d
	movl $__NR_fork, %eax
	leaq .Lfork_return(%rip), %rcx
	jmp ph_vm_client_syscall_entry
.Lfork_return:
	testq %rax, %rax
	jz .Lchild
	movq ph_vm_fixture_parent_stack(%rip), %rsp
	popq %r12
	ret
.size ph_vm_fixture_fork, .-ph_vm_fixture_fork

.globl ph_vm_fixture_clone
.type ph_vm_fixture_clone, @function
ph_vm_fixture_clone:
	pushq %r12
	movq %rdx, %r12
	movq %rcx, %rdx
	movq %rcx, %r10
	xorl %r8d, %r8d
	xorl %r9d, %r9d
	movl $__NR_clone, %eax
	leaq .Lclone_return(%rip), %rcx
	jmp ph_vm_client_syscall_entry
.Lclone_return:
	testq %rax, %rax
	jz .Lchild
	popq %r12
	ret
.size ph_vm_fixture_clone, .-ph_vm_fixture_clone

.Lchild:
	movq %r12, %rdi
	call kobox_vm_child_program
	ud2

.globl ph_vm_fixture_root
.type ph_vm_fixture_root, @function
ph_vm_fixture_root:
	movq %rdx, %rsp
	andq $-16, %rsp
	xorl %ebp, %ebp
	call kobox_vm_root_program
	ud2
.size ph_vm_fixture_root, .-ph_vm_fixture_root

.bss
.balign 8
ph_vm_fixture_parent_stack:
	.quad 0
.section .note.GNU-stack,"",@progbits
