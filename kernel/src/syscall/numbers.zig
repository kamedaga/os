const abi_root = @import("kernel_abi_root");

const ipc_buffer_abi = abi_root.ipc_buffer_abi;
const capsule_abi = abi_root.capsule_abi;
const fd_abi = abi_root.fd_abi;
const process_abi = abi_root.process_abi;
const queue_abi = abi_root.queue_abi;
const trap_abi = abi_root.trap_abi;

pub const syscall_alloc_page: u64 = 0x1;
pub const syscall_map_page: u64 = 0x2;
pub const syscall_move_cap: u64 = 0x3;
pub const syscall_drop_present: u64 = 0x4;
pub const syscall_switch_thread: u64 = 0x5;
pub const syscall_send_cap: u64 = 0x6;
pub const syscall_revoke_tree: u64 = 0x7;
pub const syscall_grant_cap: u64 = 0x8;
pub const syscall_log: u64 = 0x9;
pub const syscall_recv_cap: u64 = 0xA;
pub const syscall_map_mmio: u64 = 0xB;
pub const syscall_alloc_map_pages: u64 = 0xC;
pub const syscall_queue_submit: u64 = 0xE;
pub const syscall_queue_notify: u64 = 0xF;
pub const syscall_grant_caps_batch: u64 = 0x14;
pub const syscall_map_pages_batch: u64 = 0x15;
pub const syscall_wait_event: u64 = 0x17;
pub const syscall_grant_cap_on_endpoint: u64 = 0x24;
pub const syscall_grant_caps_batch_on_endpoint: u64 = 0x25;
pub const syscall_install_endpoint: u64 = 0x26;
pub const syscall_register_iommu_driver: u64 = 0x27;
pub const syscall_share_cap: u64 = 0x2B;
pub const syscall_signal_endpoint: u64 = 0x2C;
pub const syscall_get_tick_count: u64 = 0x2D;
pub const syscall_get_process_slot: u64 = 0x2E;
pub const syscall_install_mmio_cap: u64 = 0x2F;
pub const syscall_install_caps_batch: u64 = 0x32;
pub const syscall_publish_service_endpoint: u64 = 0x33;
pub const syscall_get_memory_stats: u64 = 0x3C;
pub const syscall_map_page_anywhere: u64 = 0x5C;
pub const syscall_alloc_map_pages_anywhere: u64 = 0x5D;
pub const syscall_ipc_call_reply_recv: u64 = 0x40;
pub const syscall_ipc_call_reply_recv_fast: u64 = 0x400;

pub const syscall_fd_close: u64 = fd_abi.syscall_fd_close;
pub const syscall_fd_dup: u64 = fd_abi.syscall_fd_dup;
pub const syscall_fd_replace: u64 = fd_abi.syscall_fd_replace;
pub const syscall_fd_get_info: u64 = fd_abi.syscall_fd_get_info;
pub const syscall_fd_set_flags: u64 = fd_abi.syscall_fd_set_flags;
pub const syscall_fd_wait: u64 = fd_abi.syscall_fd_wait;
pub const syscall_fd_poll: u64 = fd_abi.syscall_fd_poll;

pub const syscall_set_fs_base_self: u64 = process_abi.syscall_set_fs_base_self;
pub const syscall_get_process_status: u64 = process_abi.syscall_get_process_status;
pub const syscall_process_exit: u64 = process_abi.syscall_process_exit;

pub const syscall_iommu_authorize: u64 = queue_abi.syscall_iommu_authorize;
pub const syscall_command_authorize: u64 = queue_abi.syscall_command_authorize;
pub const syscall_dma_map_create: u64 = queue_abi.syscall_dma_map_create;
pub const syscall_dma_map_set_state: u64 = queue_abi.syscall_dma_map_set_state;
pub const syscall_dma_map_release: u64 = queue_abi.syscall_dma_map_release;
pub const syscall_revoke_device_cap: u64 = queue_abi.syscall_revoke_cap;
pub const syscall_derive_command_cap: u64 = queue_abi.syscall_derive_command_cap;

pub const syscall_accept_cap_transfer: u64 = 0x2A;

pub const syscall_grant_vm_object: u64 = 0x1F;
pub const syscall_release_vm_object: u64 = 0x29;
pub const syscall_drop_vm_object: u64 = 0x31;
pub const syscall_map_vm_object: u64 = 0x28;
pub const syscall_create_vm_object_from_current_pages: u64 = 0x3F;

pub const syscall_create_ipc_buffer_from_page: u64 = ipc_buffer_abi.syscall_create_ipc_buffer_from_page;
pub const syscall_grant_ipc_buffer_on_endpoint: u64 = ipc_buffer_abi.syscall_grant_ipc_buffer_on_endpoint;
pub const syscall_share_ipc_buffer_on_endpoint: u64 = ipc_buffer_abi.syscall_share_ipc_buffer_on_endpoint;
pub const syscall_accept_ipc_buffer_transfer: u64 = ipc_buffer_abi.syscall_accept_ipc_buffer_transfer;
pub const syscall_map_ipc_buffer_anywhere: u64 = ipc_buffer_abi.syscall_map_ipc_buffer_anywhere;

pub const syscall_get_rtc_unix_time: u64 = 0x3E;

pub const syscall_capsule_query: u64 = capsule_abi.syscall_capsule_query;
pub const syscall_capsule_derive_mmio: u64 = capsule_abi.syscall_capsule_derive_mmio;
pub const syscall_capsule_derive_dma_buffer: u64 = capsule_abi.syscall_capsule_derive_dma_buffer;
pub const syscall_capsule_derive_dma_mapping: u64 = capsule_abi.syscall_capsule_derive_dma_mapping;
pub const syscall_capsule_derive_dma_mapping_from_buffer: u64 = capsule_abi.syscall_capsule_derive_dma_mapping_from_buffer;
pub const syscall_capsule_derive_irq: u64 = capsule_abi.syscall_capsule_derive_irq;
pub const syscall_capsule_grant: u64 = capsule_abi.syscall_capsule_grant;
pub const syscall_capsule_revoke: u64 = capsule_abi.syscall_capsule_revoke;
pub const syscall_capsule_close: u64 = capsule_abi.syscall_capsule_close;
pub const syscall_capsule_pci_config_read: u64 = capsule_abi.syscall_capsule_pci_config_read;
pub const syscall_capsule_pci_config_write: u64 = capsule_abi.syscall_capsule_pci_config_write;
pub const syscall_capsule_pci_bar_info: u64 = capsule_abi.syscall_capsule_pci_bar_info;
pub const syscall_capsule_irq_poll: u64 = capsule_abi.syscall_capsule_irq_poll;

pub const syscall_map_abi_trap_reply_target_pages: u64 = trap_abi.syscall_map_abi_trap_reply_target_pages;
pub const syscall_copy_from_abi_trap_reply_target: u64 = trap_abi.syscall_copy_from_abi_trap_reply_target;
pub const syscall_copy_to_abi_trap_reply_target: u64 = trap_abi.syscall_copy_to_abi_trap_reply_target;
pub const syscall_set_abi_trap_reply_target_fs_base: u64 = trap_abi.syscall_set_abi_trap_reply_target_fs_base;
pub const syscall_set_abi_trap_reply_target_gs_base: u64 = trap_abi.syscall_set_abi_trap_reply_target_gs_base;
pub const syscall_protect_abi_trap_reply_target_pages: u64 = trap_abi.syscall_protect_abi_trap_reply_target_pages;
pub const syscall_unmap_abi_trap_reply_target_pages: u64 = trap_abi.syscall_unmap_abi_trap_reply_target_pages;
pub const syscall_map_abi_trap_reply_target_vm_object: u64 = trap_abi.syscall_map_abi_trap_reply_target_vm_object;
pub const syscall_reply_abi_trap_target: u64 = trap_abi.syscall_reply_abi_trap_target;
pub const syscall_copy_to_abi_trap_target: u64 = trap_abi.syscall_copy_to_abi_trap_target;
pub const syscall_set_abi_trap_target_request_page: u64 = trap_abi.syscall_set_abi_trap_target_request_page;
pub const syscall_detach_abi_trap_reply_token: u64 = trap_abi.syscall_detach_abi_trap_reply_token;
pub const syscall_copy_from_abi_trap_target: u64 = trap_abi.syscall_copy_from_abi_trap_target;
pub const syscall_reply_abi_trap_target_context: u64 = trap_abi.syscall_reply_abi_trap_target_context;
pub const syscall_share_abi_trap_reply_target_pages_to_target: u64 = trap_abi.syscall_share_abi_trap_reply_target_pages_to_target;
pub const syscall_protect_abi_trap_target_pages: u64 = trap_abi.syscall_protect_abi_trap_target_pages;
pub const syscall_unmap_abi_trap_target_pages: u64 = trap_abi.syscall_unmap_abi_trap_target_pages;
pub const syscall_interrupt_abi_trap_target: u64 = trap_abi.syscall_interrupt_abi_trap_target;

pub const syscall_batch_max_pages: usize = 256;
pub const user_log_max_bytes: usize = 256;

pub const syscall_ok: u64 = 0;
pub const syscall_err_invalid: u64 = 1;
pub const syscall_err_not_ready: u64 = 2;
pub const syscall_err_alloc: u64 = 4;
pub const syscall_err_map: u64 = 5;
pub const syscall_err_move: u64 = 6;
pub const syscall_err_drop_present: u64 = 7;
pub const syscall_err_send: u64 = 8;
pub const syscall_err_endpoint: u64 = 9;
pub const syscall_err_revoke: u64 = 10;
pub const syscall_err_grant: u64 = 11;
pub const syscall_err_empty: u64 = 13;

pub const syscall_alloc_map_drop_cap_flag: u64 = 0x2;
pub const ipc_call_flag_retain_sender: u64 = 0x1;
pub const ipc_call_flag_signal_only: u64 = 0x2;
