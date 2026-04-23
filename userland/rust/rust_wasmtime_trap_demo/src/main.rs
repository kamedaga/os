#![no_std]
#![no_main]

extern crate alloc;

use cap_std::path::Path;
use rt_alloc as _;

const ARTIFACT_PATH: &str = "/sys/wasmtime_trap_module.cwasm";

fn main() -> cap_std::Result<()> {
    cap_std::println!("rust wasmtime trap demo stage=load")?;
    let artifact =
        wasmtime_host::SerializedModuleArtifact::load_from_root(Path::new(ARTIFACT_PATH))?;
    cap_std::println!(
        "rust wasmtime trap demo stage=artifact_ready path={} bytes={} kind={}",
        artifact.path().as_str(),
        artifact.len(),
        artifact.precompiled_kind_name()
    )?;
    cap_std::println!("rust wasmtime trap demo stage=engine")?;
    let engine = wasmtime_host::default_engine()?;
    cap_std::println!("rust wasmtime trap demo stage=deserialize")?;
    let module = artifact.deserialize_module(&engine)?;
    cap_std::println!("rust wasmtime trap demo stage=instantiate")?;
    match wasmtime_host::call_zero_arg_i32_export(&engine, &module, "run") {
        Ok(value) => {
            cap_std::println!(
                "rust wasmtime trap demo observed_trap=false unexpected_result={}",
                value
            )?;
            Err(cap_std::Error::new(cap_std::ErrorKind::Other))
        }
        Err(err) => {
            cap_std::println!(
                "rust wasmtime trap demo observed_trap=true error_kind={:?} module_size={}",
                err.kind(),
                core::mem::size_of_val(&module)
            )?;
            Ok(())
        }
    }
}

cap_std::entry_point!(main);
