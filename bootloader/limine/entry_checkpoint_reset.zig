//! Diagnostic-only reset signal for production-path boot checkpoints.
//! The standalone reset probe has already shown CF9 works on the target B550;
//! this helper is compiled out of the normal kernel image.

pub fn delayedReset() noreturn {
    const start = readTsc();
    // Keep the module-load screen visible long enough to distinguish this
    // intentional reset from a firmware or CPU fault. Exact seconds vary.
    while (readTsc() -% start < 15_000_000_000) asm volatile ("pause");
    asm volatile ("cli");
    outb(0xcf9, 0x02);
    for (0..100_000) |_| asm volatile ("pause");
    outb(0xcf9, 0x06);
    // If a different machine lacks CF9, stop rather than introducing a
    // triple-fault fallback with an ambiguous reset result.
    while (true) asm volatile ("hlt");
}

fn readTsc() u64 {
    var lo: u32 = 0;
    var hi: u32 = 0;
    asm volatile ("rdtsc"
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
    );
    return (@as(u64, hi) << 32) | lo;
}

fn outb(port: u16, value: u8) void {
    asm volatile ("outb %[value], %[port]"
        :
        : [value] "{al}" (value),
          [port] "{dx}" (port),
    );
}
