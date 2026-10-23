// Pager requests are scoped to the control port and one attached VMO generation.
// No syscall accepts a destination VMO FD during supply: the port resolves the
// registered weak identity, so a creator can close its VMO FD after transfer.
pub const syscall_pager_port_create: u64 = 66;
pub const syscall_pager_attach: u64 = 67;
pub const syscall_pager_wait: u64 = 68;
pub const syscall_pager_supply: u64 = 69;
pub const syscall_pager_fail: u64 = 70;

pub const event_fault: u32 = 1;
pub const event_vmo_release: u32 = 2;
pub const access_read: u32 = 1;
pub const access_write: u32 = 2;
pub const access_execute: u32 = 4;
pub const fail_io: u32 = 1;
pub const fail_eof_or_truncate: u32 = 2;
pub const fail_pager_dead: u32 = 3;
pub const max_supply_bytes: u64 = 512 * 1024;
// A WAIT resumed by a new event returns NOT_READY (2); userland must retry
// WAIT and inspect only an event returned with OK. No event is consumed by
// this wake. This is also the bound on publicly queued fault notifications;
// excess requests remain pending in the port's unpublished FIFO.
pub const fault_queue_capacity: u32 = 256;
pub const wait_wake_retry_status: u64 = 2;

// PAGER_WAIT(port_fd=RDI, event_out=RSI, flags=RDX) returns status in RAX.
pub const Event = extern struct {
    kind: u32,
    flags: u32,
    key: u64,
    vmo_generation: u64,
    request_id: u64,
    page_offset: u64,
    page_length: u32,
    access: u32,
};

// PAGER_ATTACH(port_fd=RDI, vmo_fd=RSI, key=RDX, flags=R10) returns
// status in RAX and a nonzero opaque attach identity in RDX. Callers must
// return it unchanged for SUPPLY/FAIL; its numeric order has no meaning.
// PAGER_SUPPLY(port_fd=RDI, descriptor_va=RSI) copies exactly one 4 KiB
// aligned batch from a resident, non-pager anonymous source VMO. Status is RAX.
pub const Supply = extern struct {
    key: u64,
    vmo_generation: u64,
    request_id: u64,
    source_fd: u64,
    source_offset: u64,
    target_offset: u64,
    length: u64,
};

// PAGER_FAIL(port_fd=RDI, key=RSI, generation=RDX, request_id=R10,
// reason=R8) returns status in RAX.
comptime {
    if (@sizeOf(Event) != 48 or @alignOf(Event) != 8) @compileError("pager event ABI changed");
    if (@sizeOf(Supply) != 56 or @alignOf(Supply) != 8) @compileError("pager supply ABI changed");
}
