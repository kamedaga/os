use std::env;
use std::fs;
use std::path::Path;

const MINIMAL_RUN_WASM: &[u8] = &[
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // header
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f, // type section
    0x03, 0x02, 0x01, 0x00, // function section
    0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x00, // export section
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x07, 0x0b, // code section
];

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let output = env::args()
        .nth(1)
        .ok_or("usage: wasmtime_artifact_builder <output-path>")?;
    let output_path = Path::new(&output);
    if let Some(parent) = output_path.parent() {
        fs::create_dir_all(parent)?;
    }

    let mut config = wasmtime::Config::new();
    config.target("pulley64")?;
    config.signals_based_traps(false);
    config.memory_guard_size(0);
    config.memory_reservation(0);
    config.memory_reservation_for_growth(1 << 20);
    config.memory_init_cow(false);
    let engine = wasmtime::Engine::new(&config)?;
    let artifact = engine.precompile_module(MINIMAL_RUN_WASM)?;
    fs::write(output_path, &artifact)?;
    println!(
        "generated serialized module: {} ({} bytes)",
        output_path.display(),
        artifact.len()
    );
    Ok(())
}
