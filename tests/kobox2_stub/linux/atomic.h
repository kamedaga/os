// SPDX-License-Identifier: MIT
#ifndef KOBOX_TEST_LINUX_ATOMIC_H
#define KOBOX_TEST_LINUX_ATOMIC_H
#include <stdatomic.h>

typedef struct { atomic_int counter; } atomic_t;
void kobox_test_before_cas(atomic_t *value, int old, int next);

static inline int atomic_read(atomic_t *value)
{
	return atomic_load_explicit(&value->counter, memory_order_seq_cst);
}

static inline int atomic_cmpxchg(atomic_t *value, int old, int next)
{
	kobox_test_before_cas(value, old, next);
	atomic_compare_exchange_strong_explicit(&value->counter, &old, next,
						memory_order_seq_cst, memory_order_seq_cst);
	return old;
}
#endif
