#include <assert.h>
#include <stdio.h>
#include "../userland/personality/linux/runtime/lpr_filed.c"
int main(void)
{
    lpr_signal_thread_state_t first={0}, second={0};
    lpr_signal_thread_state_single_publish(&first);
    lpr_entry_single_stack_top=0x1000;
    assert(lpr_signal_thread_state_single_cached()==&first);
    lpr_signal_thread_state_single_invalidate();
    assert(!lpr_signal_thread_state_single_cached()&&!lpr_entry_single_stack_top);
    lpr_signal_thread_state_single_publish(&second);
    assert(lpr_signal_thread_state_single_cached()==&second);
    assert(!lpr_entry_single_stack_top); // Identity needs a fresh retained stack.
    lpr_entry_single_stack_top=0x2000;
    lpr_signal_thread_state_single_invalidate();
    assert(!lpr_entry_single_stack_top);
    puts("ENTRY_SINGLE_CACHE_PASS transition_revokes_signal_and_stack_identity");
    return 0;
}
