const mmio = @import("mmio");
fn register(borrow: *const mmio.Borrow) u32 {
    return borrow.read(u32, 0);
}
export fn reject_ram_physical() u32 {
    return register(mmio.addresses.PhysicalAddress.fromInt(4096));
}
