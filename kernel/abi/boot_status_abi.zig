pub const syscall_set_boot_status: u64 = 0x2F;

pub const status_mouse_queue_ready: u32 = 1 << 0;
pub const status_keyboard_queue_ready: u32 = 1 << 1;
pub const status_init_started: u32 = 1 << 2;
pub const status_init_manifest_begin: u32 = 1 << 3;
pub const status_init_keyboard_spawn_done: u32 = 1 << 4;
pub const status_init_mouse_spawn_done: u32 = 1 << 5;
pub const status_init_keyboard_shared_ready: u32 = 1 << 6;
pub const status_init_mouse_shared_ready: u32 = 1 << 7;
pub const status_init_compositor_arm_done: u32 = 1 << 8;
pub const status_init_first_window_spawn_done: u32 = 1 << 9;
pub const status_boot_display_accel_ready: u32 = 1 << 10;
pub const status_boot_display_scanout_prewarm_done: u32 = 1 << 11;

pub const valid_status_mask: u32 =
    status_mouse_queue_ready |
    status_keyboard_queue_ready |
    status_init_started |
    status_init_manifest_begin |
    status_init_keyboard_spawn_done |
    status_init_mouse_spawn_done |
    status_init_keyboard_shared_ready |
    status_init_mouse_shared_ready |
    status_init_compositor_arm_done |
    status_init_first_window_spawn_done |
    status_boot_display_accel_ready |
    status_boot_display_scanout_prewarm_done;
