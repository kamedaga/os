pub const syscall_vm_first: u64 = 75;
// Self-only, containing-or-next VMA. The cursor never grants access to a
// different address space; EMPTY terminates enumeration without a record.
pub const syscall_vm_query: u64 = 75;
pub const syscall_mmap: u64 = 76;
pub const syscall_munmap: u64 = 77;
pub const syscall_mprotect: u64 = 78;
pub const syscall_mremap: u64 = 79;
pub const syscall_madvise: u64 = 80;
pub const syscall_vm_last: u64 = syscall_madvise;
pub const syscall_vm_count: u64 = syscall_vm_last - syscall_vm_first + 1;

pub const prot_read: u64 = 1 << 0;
pub const prot_write: u64 = 1 << 1;
pub const prot_exec: u64 = 1 << 2;

pub const mmap_fixed: u64 = 1 << 0;
pub const mmap_fixed_noreplace: u64 = 1 << 1;
pub const mmap_private: u64 = 1 << 2;
pub const mmap_shared: u64 = 1 << 3;
pub const mmap_anonymous: u64 = 1 << 4;
pub const mmap_noreserve: u64 = 1 << 5;
// These values also permit a Linux personality to forward populate advice.
pub const madvise_populate_read: u64 = 22;
pub const madvise_populate_write: u64 = 23;
pub const mmap_pkey_shift: u64 = 8;
pub const mmap_pkey_mask: u64 = 0xF << mmap_pkey_shift;

pub const mremap_maymove: u64 = 1 << 0;
pub const mremap_fixed: u64 = 1 << 1;
pub const mremap_known_flags: u64 = mremap_maymove | mremap_fixed;

// Native mapping metadata, not a Linux procfs record. Lazy VMAs are included;
// residency and backing identities are deliberately not exposed. Each call
// observes one record, not an atomic snapshot across multiple calls.
pub const MappingInfo = extern struct {
    start: u64,
    size: u64,
    prot: u64,
    flags: u64,
    offset: u64,
};
pub const mapping_info_size: u64 = @sizeOf(MappingInfo);
comptime {
    if (@sizeOf(MappingInfo) != 40) @compileError("native mapping metadata layout");
}
