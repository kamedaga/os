const addresses = @import("addresses");

fn physicalOnly(address: addresses.PhysicalAddress) u64 {
    return address.toInt();
}

fn kernelOnly(address: addresses.KernelVirtualAddress) u64 {
    return address.toInt();
}

// This fixture MUST fail compilation. Export makes every misuse instantiated.
export fn reject_user_as_physical() u64 {
    return physicalOnly(addresses.UserVirtualAddress.fromInt(4096));
}

export fn reject_kernel_as_physical() u64 {
    return physicalOnly(addresses.KernelVirtualAddress.fromInt(4096));
}

export fn reject_physical_as_kernel() u64 {
    return kernelOnly(addresses.PhysicalAddress.fromInt(4096));
}

export fn reject_user_from_kernel_pointer(pointer: *u64) u64 {
    return addresses.UserVirtualAddress.fromPointer(pointer).toInt();
}

fn iovaOnly(address: addresses.IovaAddress) u64 {
    return address.toInt();
}
export fn reject_physical_as_iova() u64 {
    return iovaOnly(addresses.PhysicalAddress.fromInt(4096));
}
export fn reject_iova_as_physical() u64 {
    return physicalOnly(addresses.IovaAddress.fromInt(4096));
}
