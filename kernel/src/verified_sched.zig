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

pub const SchedRc = EevdfRc;

/// Allocation-independent state owned by the verified EEVDF core.  Container
/// links and CPU ownership deliberately live in the kernel adapter.
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

pub extern fn pacha_eevdf_entity_init(
    thread_id: i64,
    generation: i64,
    weight: i64,
    slice_ns: i64,
    floor_vruntime: i64,
    out: *Entity,
) EevdfRc;
pub extern fn pacha_eevdf_entity_wake(entity: *const Entity, floor_vruntime: i64, out: *Entity) EevdfRc;
pub extern fn pacha_eevdf_entity_block(entity: *const Entity, out: *Entity) EevdfRc;
pub extern fn pacha_eevdf_entity_exit(entity: *const Entity, out: *Entity) EevdfRc;
pub extern fn pacha_eevdf_entity_charge(entity: *const Entity, runtime_ns: i64, floor_vruntime: i64, out: *Entity) EevdfRc;
pub extern fn pacha_eevdf_entity_mark_running(entity: *const Entity, out: *Entity) EevdfRc;
pub extern fn pacha_eevdf_entity_finish(entity: *const Entity, out: *Entity) EevdfRc;
pub extern fn pacha_eevdf_entity_migrate(entity: *const Entity, floor_vruntime: i64, out: *Entity) EevdfRc;
pub extern fn pacha_eevdf_entity_validate(entity: *const Entity) EevdfRc;
