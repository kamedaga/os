/* Exercise the actual recvmsg boundary with stubbed transport, not a browser
 * workload or a capacity-discovery test. */
#include "../userland/personality/linux/runtime/lpr_unix/message.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

lpr_state_t lpr_state = { .process.current_pid = 100 };
static struct lpr_unix_socket socket_state = { .type = UNIX_TRANSPORT_STREAM };
static unsigned pins, unpins, logs;
static uint32_t next_flags;
static int64_t next_result = 4;
static char last_log[128];

void *lpr_memcpy(void *to, const void *from, size_t length)
{ return memcpy(to, from, length); }
int lpr_user_range_plausible(uint64_t address, uint64_t length)
{ return address >= 4096 && length <= UINT64_MAX - address; }
int lpr_fd_table_pin(lpr_fd_table_t *table, uint32_t fd, lpr_fd_pin_t *pin)
{
    assert(table == &lpr_control_fd_table && fd == 35);
    *pin = (lpr_fd_pin_t){ .ops_id = LPR_FD_OPS_UNIX, .state = &socket_state };
    ++pins;
    return 0;
}
void lpr_fd_unpin(const lpr_fd_pin_t *pin)
{ assert(pin->state == &socket_state); ++unpins; }
int lpr_unix_rights_parse(struct lpr_unix_ancillary *state)
{ (void)state; return 0; }
int lpr_unix_address_encode(uint64_t raw, uint64_t length, struct unix_address *out)
{ (void)raw; (void)length; (void)out; assert(0); return -EINVAL; }
int lpr_unix_address_copy(const struct unix_address *name, uint64_t address, uint64_t length)
{ (void)name; (void)address; (void)length; assert(0); return -EINVAL; }
int64_t lpr_unix_socket_message_iov(const lpr_fd_pin_t *pin, uint64_t vectors,
    uint64_t count, int writing, uint64_t flags, uint32_t *returned,
    struct lpr_unix_ancillary *ancillary)
{
    (void)vectors; (void)count; (void)writing; (void)flags;
    assert(pin->state == &socket_state);
    *returned = next_flags;
    ancillary->used = 24;
    return next_result;
}
int64_t lpr_pacha_syscall2(uint64_t number, uint64_t address, uint64_t length)
{
    assert(number == PACHAOS_SYSCALL_LOG && length < sizeof(last_log));
    memcpy(last_log, (void *)(uintptr_t)address, length);
    last_log[length] = 0;
    ++logs;
    return 0;
}

static void receive(int writing)
{
    char control[64];
    struct unix_linux_message message = {
        .control = (uint64_t)(uintptr_t)control, .control_length = sizeof(control),
        .flags = 0x1234,
    };
    assert(lpr_unix_socket_message(35, (uint64_t)(uintptr_t)&message, 0, writing) == next_result);
    if (!writing && next_result >= 0) {
        assert(message.flags == next_flags && message.control_length == 24);
    } else assert(message.flags == 0x1234 && message.control_length == sizeof(control));
    assert(pins == unpins);
}
int main(void)
{
    for (unsigned i = 0; i < 100000; ++i) receive(0);
    assert(!logs);
    next_flags = 8;
    next_result = -EAGAIN;
    receive(0);
    next_result = -EINTR;
    receive(0);
    next_result = -EIO;
    receive(0);
    assert(!logs); /* Failed calls cannot interpret output flags. */
    next_result = 4;
    receive(1);
    assert(!logs);
    receive(0);
    assert(logs == 1);
    assert(!strcmp(last_log, "[unix] 0000000000000064 0000000000000045 0000000000000023 000000000000006e 0000000000000040 0000000000000018\n"));
    for (unsigned i = 0; i < 100; ++i) receive(0);
    assert(logs == LPR_UNIX_DIAG_MAX_RECORDS);
    puts("unix recvmsg truncation diagnostic: quiet normal path, unchanged result/flags and bounded errors PASS");
    return 0;
}
