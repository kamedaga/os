/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>

/* Sequential state tests model only the hosted IRQ/spinlock boundary. */
unsigned long kobox_provider_irq_save_flags(void) { return 0; }
unsigned long kobox_provider_irq_save(void) { return 0; }
void kobox_provider_irq_restore(unsigned long flags) { (void)flags; }
unsigned long kobox_provider_preempt_save(void) { return 0; }
void kobox_provider_preempt_restore(unsigned long flags) { (void)flags; }
unsigned long kobox_provider_current_percpu_offset(void) { return 0; }
unsigned int __preempt_count;
unsigned long _raw_spin_lock_irqsave(void *lock) { assert(lock); return 0; }
void _raw_spin_unlock_irqrestore(void *lock, unsigned long flags) { assert(lock); (void)flags; }

extern int kobox_wifi_net_info_fixture(void);
int main(void)
{
    int result = kobox_wifi_net_info_fixture();
    if (result) fprintf(stderr, "net info fixture failed at line %d\n", result);
    assert(!result);
    puts("Net info: Ethernet zero, INFO event pump, association counter, RX flush and auth/carrier conjunction PASS");
    return 0;
}
