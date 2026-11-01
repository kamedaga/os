/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
unsigned long kobox_provider_irq_save_flags(void) { return 0; }
unsigned long kobox_provider_irq_save(void) { return 0; }
void kobox_provider_irq_restore(unsigned long flags) { (void)flags; }
unsigned long kobox_provider_preempt_save(void) { return 0; }
void kobox_provider_preempt_restore(unsigned long flags) { (void)flags; }
unsigned long kobox_provider_current_percpu_offset(void) { return 0; }
unsigned int __preempt_count;
unsigned long _raw_spin_lock_irqsave(void *lock) { assert(lock); return 0; }
void _raw_spin_unlock_irqrestore(void *lock, unsigned long flags) { assert(lock); (void)flags; }
extern int kobox_net_rx_fixture(void);
int main(void) { int n=kobox_net_rx_fixture(); if(n)fprintf(stderr,"RX fixture failed at line %d\n",n); assert(!n); puts(KOBOX_NET_RX_CONTROL ? "PASS RX measurement control preserves aggregation/timer and counts full/GSO/oversize" : "PASS real port RX: coalesced wake, 128-frame queue, 64-frame continuation, copy/drop/GSO counters, unbind/rebind, standalone features"); }
