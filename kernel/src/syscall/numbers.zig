const abi_root = @import("kernel_abi_root");

const capsule_abi = abi_root.capsule_abi;
const fd_abi = abi_root.fd_abi;
const ipc_abi = abi_root.ipc_abi;
const process_abi = abi_root.process_abi;
const runtime_abi = abi_root.runtime_abi;
const vm_abi = abi_root.vm_abi;

pub const syscall_log: u64 = 1;
pub const syscall_process_create: u64 = process_abi.syscall_process_create;
pub const syscall_process_kill: u64 = process_abi.syscall_process_kill;
pub const syscall_process_wait: u64 = process_abi.syscall_process_wait;
pub const syscall_process_exit: u64 = process_abi.syscall_process_exit;
pub const syscall_thread_create: u64 = process_abi.syscall_thread_create;
pub const syscall_thread_start: u64 = process_abi.syscall_thread_start;
pub const syscall_thread_kill: u64 = process_abi.syscall_thread_kill;
pub const syscall_thread_wait: u64 = process_abi.syscall_thread_wait;
pub const syscall_thread_exit: u64 = process_abi.syscall_thread_exit;
pub const syscall_process_signal: u64 = process_abi.syscall_process_signal;
pub const syscall_process_signal_ctl: u64 = process_abi.syscall_process_signal_ctl;
pub const syscall_process_stop: u64 = process_abi.syscall_process_stop;
pub const syscall_process_continue: u64 = process_abi.syscall_process_continue;
pub const syscall_thread_set_fs_base: u64 = process_abi.syscall_thread_set_fs_base;
pub const syscall_thread_set_gs_base: u64 = process_abi.syscall_thread_set_gs_base;
pub const syscall_process_clone: u64 = process_abi.syscall_process_clone;
pub const syscall_process_map: u64 = process_abi.syscall_process_map;
pub const syscall_process_map_batch: u64 = process_abi.syscall_process_map_batch;
pub const syscall_process_exec_from: u64 = process_abi.syscall_process_exec_from;
pub const syscall_process_memory_barrier: u64 = process_abi.syscall_process_memory_barrier;

pub const syscall_getpid: u64 = runtime_abi.syscall_getpid;
pub const syscall_gettid: u64 = runtime_abi.syscall_gettid;
pub const syscall_clock_gettime: u64 = runtime_abi.syscall_clock_gettime;
pub const syscall_nanosleep: u64 = runtime_abi.syscall_nanosleep;
pub const syscall_futex_wait: u64 = runtime_abi.syscall_futex_wait;
pub const syscall_futex_wake: u64 = runtime_abi.syscall_futex_wake;
pub const syscall_getrandom: u64 = runtime_abi.syscall_getrandom;

pub const syscall_fd_close: u64 = fd_abi.syscall_fd_close;
pub const syscall_fd_dup: u64 = fd_abi.syscall_fd_dup;
pub const syscall_fd_get_info: u64 = fd_abi.syscall_fd_get_info;
pub const syscall_fd_set_flags: u64 = fd_abi.syscall_fd_set_flags;
pub const syscall_fd_read: u64 = fd_abi.syscall_fd_read;
pub const syscall_fd_write: u64 = fd_abi.syscall_fd_write;
pub const syscall_fd_readv: u64 = fd_abi.syscall_fd_readv;
pub const syscall_fd_writev: u64 = fd_abi.syscall_fd_writev;
pub const syscall_fd_fcntl: u64 = fd_abi.syscall_fd_fcntl;
pub const syscall_fd_poll: u64 = fd_abi.syscall_fd_poll;
pub const syscall_fd_wait_many: u64 = fd_abi.syscall_fd_wait_many;
pub const syscall_fd_ioctl: u64 = fd_abi.syscall_fd_ioctl;
pub const syscall_fd_stat: u64 = fd_abi.syscall_fd_stat;
pub const syscall_eventfd_create: u64 = fd_abi.syscall_eventfd_create;
pub const syscall_pipe_create: u64 = fd_abi.syscall_pipe_create;
pub const syscall_timerfd_create: u64 = fd_abi.syscall_timerfd_create;
pub const syscall_timerfd_settime: u64 = fd_abi.syscall_timerfd_settime;
pub const syscall_timerfd_gettime: u64 = fd_abi.syscall_timerfd_gettime;
pub const syscall_vmo_create: u64 = fd_abi.syscall_vmo_create;
pub const syscall_vmo_revoke: u64 = fd_abi.syscall_vmo_revoke;
pub const syscall_mmap: u64 = vm_abi.syscall_mmap;
pub const syscall_munmap: u64 = vm_abi.syscall_munmap;
pub const syscall_mprotect: u64 = vm_abi.syscall_mprotect;
pub const syscall_mremap: u64 = vm_abi.syscall_mremap;
pub const syscall_madvise: u64 = vm_abi.syscall_madvise;

pub const syscall_ipc_endpoint_create: u64 = ipc_abi.syscall_ipc_endpoint_create;
pub const syscall_ipc_channel_create: u64 = ipc_abi.syscall_ipc_channel_create;
pub const syscall_ipc_send: u64 = ipc_abi.syscall_ipc_send;
pub const syscall_ipc_recv: u64 = ipc_abi.syscall_ipc_recv;
pub const syscall_ipc_call: u64 = ipc_abi.syscall_ipc_call;
pub const syscall_ipc_reply: u64 = ipc_abi.syscall_ipc_reply;
pub const syscall_ipc_recv_wait: u64 = ipc_abi.syscall_ipc_recv_wait;

pub const syscall_capsule_query: u64 = capsule_abi.syscall_capsule_query;
pub const syscall_capsule_derive_mmio: u64 = capsule_abi.syscall_capsule_derive_mmio;
pub const syscall_capsule_derive_dma_buffer: u64 = capsule_abi.syscall_capsule_derive_dma_buffer;
pub const syscall_capsule_derive_dma_mapping: u64 = capsule_abi.syscall_capsule_derive_dma_mapping;
pub const syscall_capsule_derive_dma_mapping_pages: u64 = capsule_abi.syscall_capsule_derive_dma_mapping_pages;
pub const syscall_capsule_derive_dma_mapping_from_buffer: u64 = capsule_abi.syscall_capsule_derive_dma_mapping_from_buffer;
pub const syscall_capsule_derive_irq: u64 = capsule_abi.syscall_capsule_derive_irq;
pub const syscall_capsule_pci_config_read: u64 = capsule_abi.syscall_capsule_pci_config_read;
pub const syscall_capsule_pci_config_write: u64 = capsule_abi.syscall_capsule_pci_config_write;
pub const syscall_capsule_pci_bar_info: u64 = capsule_abi.syscall_capsule_pci_bar_info;
pub const syscall_capsule_irq_poll: u64 = capsule_abi.syscall_capsule_irq_poll;

pub const syscall_last: u64 = syscall_capsule_irq_poll;

pub const user_log_max_bytes: usize = 256;

pub const syscall_ok: u64 = 0;
pub const syscall_err_invalid: u64 = 1;
pub const syscall_err_not_ready: u64 = 2;
pub const syscall_err_alloc: u64 = 3;
pub const syscall_err_map: u64 = 4;
pub const syscall_err_empty: u64 = 5;
pub const syscall_err_closed: u64 = 6;

test "native syscall numbers are contiguous" {
    const std = @import("std");
    const expected = [_]u64{
        syscall_log,
        syscall_process_create,
        syscall_process_kill,
        syscall_process_wait,
        syscall_process_exit,
        syscall_thread_create,
        syscall_thread_start,
        syscall_thread_kill,
        syscall_thread_wait,
        syscall_thread_exit,
        syscall_process_signal,
        syscall_process_signal_ctl,
        syscall_process_stop,
        syscall_process_continue,
        syscall_thread_set_fs_base,
        syscall_thread_set_gs_base,
        syscall_process_clone,
        syscall_process_map,
        syscall_process_map_batch,
        syscall_process_exec_from,
        syscall_process_memory_barrier,
        syscall_getpid,
        syscall_gettid,
        syscall_clock_gettime,
        syscall_nanosleep,
        syscall_futex_wait,
        syscall_futex_wake,
        syscall_getrandom,
        syscall_fd_close,
        syscall_fd_dup,
        syscall_fd_get_info,
        syscall_fd_set_flags,
        syscall_fd_read,
        syscall_fd_write,
        syscall_fd_readv,
        syscall_fd_writev,
        syscall_fd_fcntl,
        syscall_fd_poll,
        syscall_fd_wait_many,
        syscall_fd_ioctl,
        syscall_fd_stat,
        syscall_eventfd_create,
        syscall_pipe_create,
        syscall_timerfd_create,
        syscall_timerfd_settime,
        syscall_timerfd_gettime,
        syscall_vmo_create,
        syscall_vmo_revoke,
        syscall_mmap,
        syscall_munmap,
        syscall_mprotect,
        syscall_mremap,
        syscall_madvise,
        syscall_ipc_endpoint_create,
        syscall_ipc_channel_create,
        syscall_ipc_send,
        syscall_ipc_recv,
        syscall_ipc_call,
        syscall_ipc_reply,
        syscall_ipc_recv_wait,
        syscall_capsule_query,
        syscall_capsule_derive_mmio,
        syscall_capsule_derive_dma_buffer,
        syscall_capsule_derive_dma_mapping,
        syscall_capsule_derive_dma_mapping_pages,
        syscall_capsule_derive_dma_mapping_from_buffer,
        syscall_capsule_derive_irq,
        syscall_capsule_pci_config_read,
        syscall_capsule_pci_config_write,
        syscall_capsule_pci_bar_info,
        syscall_capsule_irq_poll,
    };
    try std.testing.expectEqual(@as(usize, syscall_last), expected.len);
    for (expected, 1..) |nr, index| {
        try std.testing.expectEqual(@as(u64, @intCast(index)), nr);
    }
}

test "syscall status numbers are contiguous" {
    const std = @import("std");
    const expected = [_]u64{
        syscall_ok,
        syscall_err_invalid,
        syscall_err_not_ready,
        syscall_err_alloc,
        syscall_err_map,
        syscall_err_empty,
        syscall_err_closed,
    };
    for (expected, 0..) |nr, index| {
        try std.testing.expectEqual(@as(u64, @intCast(index)), nr);
    }
}
