pub const eevdf_max_entities: usize = 256;
pub const sched_max_cpus: usize = 256;
pub const no_thread_id: i64 = 0;
pub const no_cpu: usize = sched_max_cpus;

pub const EevdfState = enum(c_int) {
    empty = 0,
    runnable = 1,
    running = 2,
    blocked = 3,
    exited = 4,
};

pub const EevdfRc = enum(c_int) {
    ok = 0,
    invalid = 1,
    full = 2,
    overflow = 3,
    state = 4,
};

pub const SchedRc = enum(c_int) {
    ok = 0,
    invalid = 1,
    full = 2,
    overflow = 3,
    state = 4,
};

pub const DecisionKind = enum(c_int) {
    none = 0,
    run_thread = 1,
    idle = 2,
};

pub const Entity = extern struct {
    thread_id: i64,
    generation: i64,
    weight: i64,
    slice_ns: i64,
    service_ns: i64,
    vruntime: i64,
    eligible_time: i64,
    deadline: i64,
    state: EevdfState,
};

pub const Runqueue = extern struct {
    entities: [eevdf_max_entities]Entity,
    entity_count: usize,
    runnable_count: usize,
    virtual_time: i64,
    min_vruntime: i64,
};

pub const PickResult = extern struct {
    rq: Runqueue,
    has_entity: c_int,
    index: usize,
    entity: Entity,
};

pub const Decision = extern struct {
    kind: DecisionKind,
    cpu_id: usize,
    thread_id: i64,
    generation: i64,
};

pub const Cpu = extern struct {
    has_current: c_int,
    current_thread_id: i64,
    current_generation: i64,
};

pub const State = extern struct {
    runqueue: Runqueue,
    cpus: [sched_max_cpus]Cpu,
    cpu_count: usize,
};

pub extern fn pacha_sched_no_decision(out: *Decision) void;
pub extern fn pacha_sched_empty_state(cpu_count: usize, out: *State) void;

pub extern fn pacha_sched_add_thread(
    sched: *State,
    thread_id: i64,
    generation: i64,
    weight: i64,
    slice_ns: i64,
    decision_out: *Decision,
    scratch: *Runqueue,
) SchedRc;

pub extern fn pacha_sched_wake_thread(
    sched: *State,
    thread_id: i64,
    decision_out: *Decision,
    scratch: *Runqueue,
) SchedRc;

pub extern fn pacha_sched_block_thread(
    sched: *State,
    thread_id: i64,
    decision_out: *Decision,
    scratch: *Runqueue,
) SchedRc;

pub extern fn pacha_sched_exit_thread(
    sched: *State,
    thread_id: i64,
    decision_out: *Decision,
    scratch: *Runqueue,
) SchedRc;

pub extern fn pacha_sched_on_timer(
    sched: *State,
    cpu_id: usize,
    runtime_ns: i64,
    decision_out: *Decision,
    scratch: *Runqueue,
) SchedRc;

pub extern fn pacha_sched_pick(
    sched: *State,
    cpu_id: usize,
    decision_out: *Decision,
    pick_scratch: *PickResult,
    scratch: *Runqueue,
) SchedRc;

pub extern fn pacha_sched_finish_current(
    sched: *State,
    cpu_id: usize,
    decision_out: *Decision,
    scratch: *Runqueue,
) SchedRc;
