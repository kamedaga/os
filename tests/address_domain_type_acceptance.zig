const addresses = @import("addresses");

fn physicalOnly(address: addresses.PhysicalAddress) u64 {
    return address.toInt();
}

fn kernelOnly(address: addresses.KernelVirtualAddress) u64 {
    return address.toInt();
}

export fn accept_physical() u64 {
    return physicalOnly(addresses.PhysicalAddress.fromInt(4096));
}

export fn accept_kernel(pointer: *u64) u64 {
    return kernelOnly(addresses.KernelVirtualAddress.fromPointer(pointer));
}

export fn accept_user() u64 {
    return addresses.UserVirtualAddress.fromInt(4096).toInt();
}
