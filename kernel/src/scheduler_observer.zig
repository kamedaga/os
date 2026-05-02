const interrupts = @import("interrupts.zig");

pub const IdleObserver = *const fn (usize) callconv(.c) void;
pub const IdleSchedulerPoll = *const fn (usize) callconv(.c) void;
pub const IdleUserEntryPoll = *const fn (usize, *UserEntry) callconv(.c) bool;

pub const UserEntry = extern struct {
    cpu_slot: usize,
    thread_index: usize,
    cr3: u64,
    fs_base: u64,
    frame: interrupts.TrapFrame,
};

var idle_observer: ?IdleObserver = null;
var idle_scheduler_poll: ?IdleSchedulerPoll = null;
var idle_user_entry_poll: ?IdleUserEntryPoll = null;

pub fn registerIdleObserver(observer: IdleObserver) void {
    idle_observer = observer;
}

pub fn registerIdleSchedulerPoll(poll: IdleSchedulerPoll) void {
    idle_scheduler_poll = poll;
}

pub fn registerIdleUserEntryPoll(poll: IdleUserEntryPoll) void {
    idle_user_entry_poll = poll;
}

pub fn observeIdle(cpu_slot: usize) void {
    if (idle_observer) |observer| observer(cpu_slot);
}

pub fn pollIdleScheduler(cpu_slot: usize) void {
    if (idle_scheduler_poll) |poll| poll(cpu_slot);
}

pub fn claimIdleUserEntry(cpu_slot: usize, out_entry: *UserEntry) bool {
    if (idle_user_entry_poll) |poll| return poll(cpu_slot, out_entry);
    return false;
}
