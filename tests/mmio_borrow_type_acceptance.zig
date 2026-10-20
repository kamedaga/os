const mmio = @import("mmio");
export fn read_borrowed_register(pointer: [*]u8) u32 {
    const borrow = mmio.mappedAfterValidation(pointer, 4, .uc);
    return borrow.read(u32, 0);
}
