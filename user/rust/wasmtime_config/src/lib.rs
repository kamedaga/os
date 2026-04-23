#![no_std]

pub const WASMTIME_TARGET_TRIPLE: &str = "pulley64";
pub const SIGNALS_BASED_TRAPS: bool = false;
pub const MEMORY_GUARD_SIZE: u64 = 0;
pub const MEMORY_RESERVATION: u64 = 0;
pub const MEMORY_RESERVATION_FOR_GROWTH: u64 = 1 << 20;
pub const MEMORY_INIT_COW: bool = false;

pub fn configure_capabilityos_target(
    config: &mut wasmtime::Config,
) -> core::result::Result<(), wasmtime::Error> {
    config.target(WASMTIME_TARGET_TRIPLE)?;
    config.signals_based_traps(SIGNALS_BASED_TRAPS);
    config.memory_guard_size(MEMORY_GUARD_SIZE);
    config.memory_reservation(MEMORY_RESERVATION);
    config.memory_reservation_for_growth(MEMORY_RESERVATION_FOR_GROWTH);
    config.memory_init_cow(MEMORY_INIT_COW);
    Ok(())
}
