#include "unixd/notify.h"

int unix_wait_arm(struct unix_wait_bank *bank, const uint64_t *first_changes,
    const uint64_t *second_changes, uint32_t notification, uint32_t attempt,
    struct unix_wait_registration *out)
{
    if (!bank || !first_changes || !second_changes || !out || !notification || !attempt)
        return -22;
    *out = (struct unix_wait_registration){
        .token = (uint64_t)notification | ((uint64_t)attempt << 32),
        .changes = { __atomic_load_n(first_changes, __ATOMIC_SEQ_CST),
            __atomic_load_n(second_changes, __ATOMIC_SEQ_CST) },
    };
    for (uint32_t slot = 0; slot < UNIX_WAIT_SLOTS; slot++) {
        uint64_t empty = 0;
        if (__atomic_compare_exchange_n(&bank->armed[slot], &empty, out->token,
            0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            out->slot = slot;
            return 0;
        }
    }
    out->token = 0;
    return -11;
}

int unix_wait_changed(const uint64_t *first_changes, const uint64_t *second_changes,
    const struct unix_wait_registration *registration)
{
    return !first_changes || !second_changes || !registration || !registration->token ||
        __atomic_load_n(first_changes, __ATOMIC_SEQ_CST) != registration->changes[0] ||
        __atomic_load_n(second_changes, __ATOMIC_SEQ_CST) != registration->changes[1];
}

void unix_wait_disarm(struct unix_wait_bank *bank,
    const struct unix_wait_registration *registration)
{
    if (!bank || !registration || registration->slot >= UNIX_WAIT_SLOTS ||
        !registration->token) return;
    uint64_t expected = registration->token;
    (void)__atomic_compare_exchange_n(&bank->armed[registration->slot], &expected,
        0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void unix_wait_remove(struct unix_wait_bank *bank, uint32_t notification)
{
    if (!bank || !notification) return;
    for (unsigned slot = 0; slot < UNIX_WAIT_SLOTS; slot++) {
        uint64_t token = __atomic_load_n(&bank->armed[slot], __ATOMIC_SEQ_CST);
        if ((uint32_t)token == notification)
            (void)__atomic_compare_exchange_n(&bank->armed[slot], &token,
                0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}

void unix_notify_scan(const struct unix_wait_bank *bank, unix_notify_fn send,
    void *context)
{
    if (!bank || !send) return;
    for (unsigned slot = 0; slot < UNIX_WAIT_SLOTS; slot++) {
        const uint64_t token = __atomic_load_n(&bank->armed[slot], __ATOMIC_SEQ_CST);
        if ((uint32_t)token && (uint32_t)(token >> 32)) send(context, token);
    }
}
