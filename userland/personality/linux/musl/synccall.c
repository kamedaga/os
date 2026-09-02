#include "pthread_impl.h"

static void dummy_0(void)
{
}

weak_alias(dummy_0, __tl_lock);
weak_alias(dummy_0, __tl_unlock);

void __synccall(void (*func)(void *), void *ctx)
{
	/* LPR owns credentials and resource limits as process-wide state.  Linux
	 * musl normally repeats these operations in every kernel thread because
	 * Linux stores credentials per thread.  Repeating an LPR operation would
	 * only update the same shared state, so execute it exactly once. */
	func(ctx);
}
