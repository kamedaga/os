// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include "../kobox2/linux-sandbox/kobox/boot/receive_return.h"

static int inject_old = -1;
static int inject_new;
static int inject_value;

void kobox_test_before_cas(atomic_t *value, int old, int next)
{
	if (old != inject_old || next != inject_new)
		return;
	atomic_store_explicit(&value->counter, inject_value,
				      memory_order_seq_cst);
	inject_old = -1;
}

static void check(int initial, enum kobox_receive_return_result expected,
		  int final)
{
	atomic_t state;
	atomic_init(&state.counter, initial);
	assert(kobox_receive_reclaim(&state) == expected);
	assert(atomic_read(&state) == final);
}

int main(void)
{
	check(RECEIVE_EXECUTING, KOBOX_RECEIVE_OWNED, RECEIVE_OWNER);
	check(RECEIVE_LENT_IDLE, KOBOX_RECEIVE_OWNED, RECEIVE_OWNER);
	check(RECEIVE_LENT_ADVANCING, KOBOX_RECEIVE_WAIT, RECEIVE_RETURNING);

	/* Deputy completes ADVANCING -> IDLE after the owner observed
	 * ADVANCING but before its CAS. The old two-CAS sequence stranded it. */
	inject_old = RECEIVE_LENT_ADVANCING;
	inject_new = RECEIVE_RETURNING;
	inject_value = RECEIVE_LENT_IDLE;
	check(RECEIVE_LENT_ADVANCING, KOBOX_RECEIVE_OWNED, RECEIVE_OWNER);
	assert(inject_old == -1);

	/* The inverse transition during the idle CAS must also retry. */
	inject_old = RECEIVE_LENT_IDLE;
	inject_new = RECEIVE_OWNER;
	inject_value = RECEIVE_LENT_ADVANCING;
	check(RECEIVE_LENT_IDLE, KOBOX_RECEIVE_WAIT, RECEIVE_RETURNING);
	assert(inject_old == -1);
	puts("kobox2 receive return: PASS");
	return 0;
}
