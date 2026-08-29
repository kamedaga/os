pub const syscall_vm_first: u64 = 50;
pub const syscall_mmap: u64 = 50;
pub const syscall_munmap: u64 = 51;
pub const syscall_mprotect: u64 = 52;
pub const syscall_mremap: u64 = 53;
pub const syscall_madvise: u64 = 54;
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
pub const mmap_pkey_shift: u64 = 8;
pub const mmap_pkey_mask: u64 = 0xF << mmap_pkey_shift;

pub const mremap_maymove: u64 = 1 << 0;
pub const mremap_fixed: u64 = 1 << 1;
pub const mremap_known_flags: u64 = mremap_maymove | mremap_fixed;
