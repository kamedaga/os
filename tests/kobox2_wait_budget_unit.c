/* SPDX-License-Identifier: GPL-2.0-only */
#include "boot/wait_budget.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void)
{
	struct kobox_wait_budget budget = {0};
	unsigned long returned = 250;

	/* Each upstream call consumes 20 ticks. The 25-tick gaps between
	 * calls are deliberately not charged to its relative timeout.
	 */
	for (unsigned int round = 0; round < 4; round++) {
		budget.asleep = 5 + round * 45;
		kobox_wait_budget_wake(&budget, budget.asleep + 20);
		returned -= 20;
		assert(kobox_wait_budget_remaining(&budget, 250) == returned);
	}
	assert(returned == 170);
	/* The old continuous-interval oracle incorrectly rejected 170. */
	assert(returned > 250 - (160 - 5));
	/* Resetting to the original timeout on the final call still fails. */
	assert(250 - 20 > kobox_wait_budget_remaining(&budget, 250));
	assert(kobox_wait_budget_remaining(&budget, 80) == 0);
	assert(kobox_wait_budget_remaining(&budget, 79) == 0);
	budget.asleep = ULONG_MAX - 2;
	budget.slept = 0;
	kobox_wait_budget_wake(&budget, 3);
	assert(budget.slept == 6);
	assert(kobox_wait_budget_remaining(&budget, 10) == 4);
	puts("KOBOX_WAIT_BUDGET_UNIT=PASS rearm-gap,reset-rejected,expiry,wrap");
	return 0;
}
