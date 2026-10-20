pub const smp_profile_enabled = true;
pub var profile_test_cpu: usize = 0;
pub var profile_test_time: u64 = 0;
test {
    _ = @import("lock_profile.zig");
}
