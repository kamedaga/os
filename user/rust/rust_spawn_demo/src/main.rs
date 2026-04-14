#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use core::fmt::Write;
use core::hint::spin_loop;
use core::ptr::{read_volatile, write_volatile};
use rt_alloc as _;
use rt_core::entry_point;
use rt_core::{SyscallError, syscall};
use rt_handle::{
    ExecImageRights, ExecImageToken, SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE, SpawnBuilder,
    VmObjectRights, VmObjectToken, fixed_va,
};

const CONFIG_MAGIC: u64 = 0x5253_5044_454D_4F31;
const CONFIG_VERSION: u64 = 1;
const CONFIG_STATE_READY: u64 = 1;
const CONFIG_STATE_FAILED: u64 = 2;
const CONFIG_WAIT_SPINS: usize = 20_000_000;
const CONFIG_IDX_MAGIC: usize = 0;
const CONFIG_IDX_VERSION: usize = 1;
const CONFIG_IDX_STATE: usize = 2;
const CONFIG_IDX_REMAINING_DEPTH: usize = 3;
const CONFIG_IDX_VM_OBJECT_TOKEN: usize = 4;
const CONFIG_IDX_LINEAGE: usize = 5;
const CONFIG_PAGE_SOURCE_CANDIDATES: [u64; 8] = [
    0x3F10_0000,
    0x3F10_1000,
    0x3F10_2000,
    0x3F10_3000,
    0x3F10_4000,
    0x3F10_5000,
    0x3F10_6000,
    0x3F10_7000,
];

struct DemoConfig {
    state: u64,
    remaining_depth: u64,
    lineage: u64,
    vm_object_token: Option<VmObjectToken>,
}

fn config_ptr_at(va: u64) -> *mut u64 {
    va as *mut u64
}

unsafe fn read_config_word(va: u64, index: usize) -> u64 {
    let ptr = unsafe { config_ptr_at(va).add(index) };
    unsafe { read_volatile(ptr) }
}

unsafe fn write_config_word(va: u64, index: usize, value: u64) {
    let ptr = unsafe { config_ptr_at(va).add(index) };
    unsafe { write_volatile(ptr, value) };
}

fn load_demo_config(va: u64) -> Option<DemoConfig> {
    // SAFETY: The demo config page is bootstrapped at a fixed user VA by the launcher.
    unsafe {
        if read_config_word(va, CONFIG_IDX_MAGIC) != CONFIG_MAGIC {
            return None;
        }
        if read_config_word(va, CONFIG_IDX_VERSION) != CONFIG_VERSION {
            return None;
        }

        Some(DemoConfig {
            state: read_config_word(va, CONFIG_IDX_STATE),
            remaining_depth: read_config_word(va, CONFIG_IDX_REMAINING_DEPTH),
            lineage: read_config_word(va, CONFIG_IDX_LINEAGE),
            vm_object_token: VmObjectToken::from_raw(read_config_word(
                va,
                CONFIG_IDX_VM_OBJECT_TOKEN,
            )),
        })
    }
}

fn wait_for_demo_config() -> Result<DemoConfig, &'static str> {
    for _ in 0..CONFIG_WAIT_SPINS {
        if let Some(config) = load_demo_config(fixed_va::STANDARD_CONFIG_TARGET_VA) {
            match config.state {
                CONFIG_STATE_READY => return Ok(config),
                CONFIG_STATE_FAILED => return Err("config failed"),
                _ => {}
            }
        }
        spin_loop();
    }
    Err("config timeout")
}

fn init_demo_config_page(
    va: u64,
    state: u64,
    remaining_depth: u64,
    vm_token_raw: u64,
    lineage: u64,
) {
    // SAFETY: `va` comes from a successfully mapped owned page in the current process.
    unsafe {
        write_config_word(va, CONFIG_IDX_MAGIC, CONFIG_MAGIC);
        write_config_word(va, CONFIG_IDX_VERSION, CONFIG_VERSION);
        write_config_word(va, CONFIG_IDX_REMAINING_DEPTH, remaining_depth);
        write_config_word(va, CONFIG_IDX_VM_OBJECT_TOKEN, vm_token_raw);
        write_config_word(va, CONFIG_IDX_LINEAGE, lineage);
        write_config_word(va, CONFIG_IDX_STATE, state);
    }
}

fn alloc_owned_config_source_page() -> Result<u64, SyscallError> {
    let mut last_error = SyscallError::Map;
    for candidate_va in CONFIG_PAGE_SOURCE_CANDIDATES {
        let status = syscall::call4(syscall::ALLOC_MAP_PAGES, candidate_va, 1, 1, 0);
        if status == syscall::OK {
            return Ok(candidate_va);
        }
        let err = SyscallError::from_error_raw(status);
        if err != SyscallError::Map {
            last_error = err;
        }
    }
    Err(last_error)
}

fn log_and_abort(mut message: String) -> ! {
    message.push('\n');
    rt_core::log(&message);
    rt_core::abort()
}

fn main() -> ! {
    let config = match wait_for_demo_config() {
        Ok(config) => config,
        Err(reason) => log_and_abort(String::from(reason)),
    };

    let mut message = String::from("rust spawn demo");
    let _ = write!(
        &mut message,
        " depth={} lineage={}",
        config.remaining_depth, config.lineage
    );

    let self_vm_object = match config.vm_object_token {
        Some(token) => token,
        None => {
            message.push_str(" missing_vm_object_token");
            log_and_abort(message);
        }
    };
    let self_exec =
        match ExecImageToken::install_from_vm_object(self_vm_object, ExecImageRights::EXEC) {
            Ok(token) => token,
            Err(err) => {
                let _ = write!(&mut message, " install_exec_failed={err:?}");
                log_and_abort(message);
            }
        };

    if config.remaining_depth == 0 {
        message.push_str(" leaf");
        log_and_abort(message);
    }

    let child_remaining_depth = config.remaining_depth - 1;
    let child_lineage = config.lineage + 1;
    let child_config_source_va = match alloc_owned_config_source_page() {
        Ok(va) => va,
        Err(err) => {
            let _ = write!(&mut message, " alloc_config_failed={err:?}");
            log_and_abort(message);
        }
    };
    init_demo_config_page(
        child_config_source_va,
        CONFIG_STATE_READY,
        child_remaining_depth,
        0,
        child_lineage,
    );

    let mut spawner = Box::new(SpawnBuilder::new(self_exec));
    if let Err(err) = spawner.push_page(
        child_config_source_va,
        fixed_va::STANDARD_CONFIG_TARGET_VA,
        SPAWN_FLAG_BOOTSTRAP_PAGE_WRITABLE,
    ) {
        let _ = write!(&mut message, " push_page_failed={err:?}");
        log_and_abort(message);
    }
    if let Err(err) = spawner.push_vm_object_cap(
        self_vm_object,
        fixed_va::STANDARD_CONFIG_TARGET_VA + (CONFIG_IDX_VM_OBJECT_TOKEN as u64) * 8,
        VmObjectRights::READ.union(VmObjectRights::GRANT),
    ) {
        let _ = write!(&mut message, " push_vm_object_failed={err:?}");
        log_and_abort(message);
    }

    let child = match spawner.spawn() {
        Ok(child) => child,
        Err(err) => {
            let _ = write!(&mut message, " spawn_failed={err:?}");
            log_and_abort(message);
        }
    };
    let _ = write!(&mut message, " spawned_child={}", child.process_slot());
    log_and_abort(message)
}

entry_point!(main);
