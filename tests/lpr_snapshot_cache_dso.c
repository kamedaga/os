/* SPDX-License-Identifier: MIT */
int snapshot_counter=7;
int *snapshot_counter_pointer=&snapshot_counter;
int snapshot_bump(void) { return ++*snapshot_counter_pointer; }
long snapshot_pid(void)
{
    long result=39;
    __asm__ volatile("syscall":"+a"(result)::"rcx","r11","memory");
    return result;
}
