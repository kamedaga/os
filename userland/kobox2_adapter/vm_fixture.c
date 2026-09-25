// SPDX-License-Identifier: GPL-2.0-only
/* Shared guest workloads use the existing LPR capture/return transport. */
#include "vm_fixture.h"
#include "tests/clients/vm_program.h"

extern uint64_t ph_vm_client_linux_call(uint64_t number, const uint64_t arguments[6]);
extern uint64_t ph_vm_fixture_fork(uint64_t base);
extern uint64_t ph_vm_fixture_clone(uint64_t flags, uint64_t stack,
	uint64_t base, uint64_t tid_address);
extern _Noreturn void ph_vm_fixture_root(uint64_t base, unsigned int cpu, uint64_t stack);

uint64_t kobox_vm_program_syscall(uint64_t number, uint64_t a0, uint64_t a1,
	uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	const uint64_t arguments[6] = {a0, a1, a2, a3, a4, a5};

	return ph_vm_client_linux_call(number, arguments);
}

uint64_t ph_vm_fixture_access(unsigned int operation, uint64_t base, uint64_t value)
{
	switch (operation) {
	case 7:
		return ph_vm_fixture_fork(base);
	case 8:
		return kobox_vm_stack_access(base, value);
	case 9:
	case 10:
		return ph_vm_fixture_clone(value, base + 4 * 4096, base,
			operation == 10 ? base + 296 : 0);
	case 11:
		ph_vm_fixture_root(base, value, base + 32 * 4096);
	default:
		__builtin_trap();
	}
}
