// Shared error vocabulary without importing resource owners. Keeping this
// leaf separate lets allocator algorithms avoid a dependency on KernelState.
pub const KernelError = error{
    RegionNotFound,
    EndpointNotFound,
    MailboxEmpty,
    MailboxFull,
    RevokeOverflow,
    NoDmaRight,
    InvalidState,
    TableFull,
    EmptyRegionSet,
    FreeRangeOverlap,
    PageQuarantined,
    TooManyFreePages,
    OutOfFreePages,
};
