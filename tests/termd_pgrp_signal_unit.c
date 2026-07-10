#include <stdint.h>
#include <stdio.h>

#include "kobox/module.h"
#include "kobox/shim.h"

static unsigned char fake_task[4096];

kb_module_t *kb_loader_active_module(void)
{
    return 0;
}

void *kb_loader_module_current_task(const kb_module_t *module)
{
    (void)module;
    return fake_task;
}

int main(void)
{
    enum { expected_pgrp = 4321, expected_signal = 2 };
    void *pgrp = kb_find_vpid(expected_pgrp);
    if (pgrp == 0 || kb_kill_pgrp(pgrp, expected_signal, 1) != 0) {
        return 1;
    }

    int pgrp_id = 0;
    int signal = 0;
    uint64_t sequence = 0;
    if (kb_take_pending_pgrp_signal(0, &pgrp_id, &signal, &sequence) != 1 ||
        pgrp_id != expected_pgrp || signal != expected_signal || sequence == 0)
    {
        return 2;
    }

    uint64_t next_sequence = 0;
    if (kb_take_pending_pgrp_signal(sequence, &pgrp_id, &signal, &next_sequence) != 0 ||
        next_sequence != sequence)
    {
        return 3;
    }
    if (kb_kill_pgrp(pgrp, 65, 0) != -22) {
        return 4;
    }

    puts("TERMD_PGRP_SIGNAL_UNIT=OK");
    return 0;
}
