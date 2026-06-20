pub const abi_version: u16 = 1;

pub const fd_kind_schedctl: u64 = 15;
pub const fd_kind_sched_event: u64 = 16;

pub const event_thread_ready: u16 = 1;
pub const event_thread_blocked: u16 = 2;
pub const event_thread_exited: u16 = 3;
pub const event_thread_yield: u16 = 4;
pub const event_tick: u16 = 5;
pub const event_cpu_idle: u16 = 6;

pub const flag_none: u64 = 0;
pub const flag_preempted: u64 = 1 << 0;
pub const flag_bootstrap: u64 = 1 << 1;

pub const ioctl_query_caps: u64 = 0x53434801;
pub const ioctl_commit: u64 = 0x53434802;
pub const ioctl_set_weight: u64 = 0x53434803;

pub const no_thread: u64 = 0;

pub const sched_event_size: u64 = 64;
pub const sched_commit_size: u64 = 40;
pub const sched_weight_size: u64 = 40;
pub const sched_caps_size: u64 = 48;

pub const sched_event_size_offset: u64 = 0;
pub const sched_event_version_offset: u64 = 4;
pub const sched_event_type_offset: u64 = 6;
pub const sched_event_sequence_offset: u64 = 8;
pub const sched_event_cpu_id_offset: u64 = 16;
pub const sched_event_thread_id_offset: u64 = 24;
pub const sched_event_generation_offset: u64 = 32;
pub const sched_event_runtime_ns_offset: u64 = 40;
pub const sched_event_weight_offset: u64 = 48;
pub const sched_event_slice_ns_offset: u64 = 56;

pub const sched_commit_size_offset: u64 = 0;
pub const sched_commit_version_offset: u64 = 4;
pub const sched_commit_cpu_id_offset: u64 = 8;
pub const sched_commit_flags_offset: u64 = 12;
pub const sched_commit_thread_id_offset: u64 = 16;
pub const sched_commit_generation_offset: u64 = 24;
pub const sched_commit_sequence_offset: u64 = 32;

pub const sched_weight_size_offset: u64 = 0;
pub const sched_weight_version_offset: u64 = 4;
pub const sched_weight_thread_id_offset: u64 = 8;
pub const sched_weight_generation_offset: u64 = 16;
pub const sched_weight_weight_offset: u64 = 24;
pub const sched_weight_slice_ns_offset: u64 = 32;

pub const sched_caps_size_offset: u64 = 0;
pub const sched_caps_version_offset: u64 = 4;
pub const sched_caps_event_size_offset: u64 = 8;
pub const sched_caps_commit_size_offset: u64 = 16;
pub const sched_caps_weight_size_offset: u64 = 24;
pub const sched_caps_flags_offset: u64 = 32;
