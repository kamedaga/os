const acpi_dmar = @import("acpi_dmar.zig");
const kernel_log = @import("kernel_log.zig");

pub const Discovery = struct {
    table_paddr: u64,
    info: acpi_dmar.DmarInfo,
};

var discovery_state: ?Discovery = null;

pub fn kernelStaticStorageStartAddr() usize {
    return @intFromPtr(&discovery_state);
}

pub fn kernelStaticStorageEndAddr() usize {
    return @intFromPtr(&discovery_state) + @sizeOf(@TypeOf(discovery_state));
}

pub fn init(rsdp_paddr: u64) void {
    discovery_state = null;
    const table = acpi_dmar.findDmar(rsdp_paddr) orelse {
        kernel_log.write("vtd: DMAR not found\n");
        return;
    };
    const info = acpi_dmar.parseDmar(table.bytes) catch |err| {
        kernel_log.writeFmt("vtd: DMAR invalid paddr=0x{x} error={s}\n", .{ table.paddr, @errorName(err) });
        return;
    };
    discovery_state = .{
        .table_paddr = table.paddr,
        .info = info,
    };

    kernel_log.writeFmt(
        "vtd: DMAR paddr=0x{x} host_address_width={} raw_haw={} flags=0x{x} drhds={}\n",
        .{ table.paddr, info.hostAddressWidthBits(), info.host_address_width, info.flags, info.drhd_count },
    );
    for (info.drhds[0..info.drhd_count]) |drhd| {
        kernel_log.writeFmt(
            "vtd: drhd base=0x{x} segment={} include_pci_all={}\n",
            .{ drhd.register_base, drhd.segment, @intFromBool(drhd.include_pci_all) },
        );
    }
}

pub fn discovered() ?*const Discovery {
    if (discovery_state) |*value| return value;
    return null;
}

pub fn isActive() bool {
    return false;
}

pub fn isAddressableRange(paddr: u64, size: u64) bool {
    _ = paddr;
    _ = size;
    return true;
}

pub fn mapRange(iova: u64, paddr: u64, size: u64) bool {
    _ = iova;
    _ = paddr;
    _ = size;
    return true;
}

pub fn unmapRange(iova: u64, size: u64) void {
    _ = iova;
    _ = size;
}
