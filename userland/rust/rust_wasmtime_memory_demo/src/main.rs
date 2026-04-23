#![no_std]
#![no_main]

extern crate alloc;

use cap_std::path::Path;
use rt_alloc as _;

const ARTIFACT_PATH: &str = "/sys/wasmtime_memory_module.cwasm";

fn main() -> cap_std::Result<()> {
    cap_std::println!("rust wasmtime memory demo stage=load")?;
    let artifact =
        wasmtime_host::SerializedModuleArtifact::load_from_root(Path::new(ARTIFACT_PATH))?;
    cap_std::println!(
        "rust wasmtime memory demo stage=artifact_ready path={} bytes={} kind={}",
        artifact.path().as_str(),
        artifact.len(),
        artifact.precompiled_kind_name()
    )?;
    cap_std::println!("rust wasmtime memory demo stage=engine")?;
    let engine = wasmtime_host::default_engine()?;
    cap_std::println!("rust wasmtime memory demo stage=deserialize")?;
    let module = artifact.deserialize_module(&engine)?;
    cap_std::println!("rust wasmtime memory demo stage=instantiate")?;
    let run_result =
        wasmtime_host::call_zero_arg_i32_export_with_host_log_memory(&engine, &module, "run")?;
    cap_std::println!(
        "rust wasmtime memory demo path={} bytes={} kind={} module_size={} run_result={}",
        artifact.path().as_str(),
        artifact.len(),
        artifact.precompiled_kind_name(),
        core::mem::size_of_val(&module),
        run_result
    )?;
    Ok(())
}

cap_std::entry_point!(main);
