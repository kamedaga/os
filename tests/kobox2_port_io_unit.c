/* SPDX-License-Identifier: MIT */
#include "../userland/kobox2_adapter/port_io.h"
#include <assert.h>
#include <stdio.h>

struct host { unsigned calls; long result; uint16_t port; bool writing; uint8_t value; };
static long operation(void *context, uint16_t port, bool writing, uint8_t value) {
    struct host *host = context;
    ++host->calls; host->port = port; host->writing = writing; host->value = value;
    return host->result;
}

int main(void) {
    const uint64_t original = UINT64_C(0x123456789abcdef0);
    struct host host = {.result = 0xa5};
    uint64_t ax = original, ip = 0x1000;
    assert(ph_scalar_port_io(0xec, 13, 0, &ax, UINT64_C(0x123403cc), &ip, operation, &host));
    assert(ax == UINT64_C(0x123456789abcdea5) && ip == 0x1001);
    assert(host.calls == 1 && !host.writing && host.port == 0x3cc);
    ax = original; ip = 0x2000; host.result = 0;
    assert(ph_scalar_port_io(0xee, 13, 0, &ax, 0x3c2, &ip, operation, &host));
    assert(host.writing && host.value == 0xf0 && host.port == 0x3c2);
    assert(ax == original && ip == 0x2001);
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        if (opcode == 0xec || opcode == 0xee) continue;
        ax = original; ip = 0x3000; const unsigned calls = host.calls;
        assert(!ph_scalar_port_io(opcode, 13, 0, &ax, 0x3cc, &ip, operation, &host));
        assert(host.calls == calls && ax == original && ip == 0x3000);
    }
    for (unsigned vector = 0; vector < 32; ++vector) {
        if (vector == 13) continue;
        const unsigned calls = host.calls;
        assert(!ph_scalar_port_io(0xec, vector, 0, &ax, 0x3cc, &ip, operation, &host));
        assert(host.calls == calls);
    }
    assert(!ph_scalar_port_io(0xec, 13, 1, &ax, 0x3cc, &ip, operation, &host));
    assert(!ph_scalar_port_io(0xec, 13, 0, &ax, 0x3cc, &ip, NULL, &host));
    ip = UINT64_MAX;
    assert(!ph_scalar_port_io(0xec, 13, 0, &ax, 0x3cc, &ip, operation, &host));
    for (unsigned writing = 0; writing < 2; ++writing) {
        for (unsigned failure = 0; failure < 3; ++failure) {
            host.result = failure == 0 ? -1 : failure == 1 ? -2 : writing ? 2 : 256;
            ax = original; ip = 0x4000;
            assert(!ph_scalar_port_io(writing ? 0xee : 0xec, 13, 0, &ax, 0x3cc, &ip, operation, &host));
            assert(ax == original && ip == 0x4000);
        }
    }
    puts("scalar port IO: IN/OUT register semantics and rejected operations PASS");
}
