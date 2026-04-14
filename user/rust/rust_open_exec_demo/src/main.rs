#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write;

use rt_alloc as _;
use rt_core::entry_point;
use rt_handle::{ExecImageRights, FsConnectionId, HandleTable, SpawnBuilder};
use rt_io::{MonotonicClock, PersistentFsClient};

const EXEC_PATH: &str = "/cmd/pie_user.elf";

fn abort_with(mut message: String) -> ! {
    message.push('\n');
    rt_core::log(&message);
    rt_core::abort()
}

fn log_stage(stage: &str) {
    let mut message = String::from("rust open_exec demo stage=");
    message.push_str(stage);
    message.push('\n');
    rt_core::log(&message);
}

fn main() -> ! {
    log_stage("start");

    let mut message = String::from("rust open_exec demo");
    let clock = MonotonicClock::new();
    let _ = write!(&mut message, " tick={}", clock.now_ticks());

    let mut client = match PersistentFsClient::connect_from_shadow(FsConnectionId::new(1)) {
        Ok(client) => client,
        Err(err) => {
            let _ = write!(&mut message, " connect_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("connected");

    let root = match client.root_dir() {
        Ok(dir) => dir,
        Err(err) => {
            let _ = write!(&mut message, " root_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("root_ready");

    let file = match client.lookup_file(root, EXEC_PATH) {
        Ok(file) => file,
        Err(err) => {
            let _ = write!(&mut message, " lookup_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("lookup_ready");

    let exec = match client.open_exec(file) {
        Ok(exec) => exec,
        Err(err) => {
            let _ = write!(&mut message, " open_exec_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("open_exec_ready");

    let mut handles = HandleTable::new();
    let root_handle =
        handles.insert_dir(client.connection_id(), root.token(), root.assumed_rights());
    let file_handle =
        handles.insert_vnode_file(client.connection_id(), file.token(), file.assumed_rights());
    let exec_handle = handles.insert_exec_image(exec.token, ExecImageRights::EXEC);

    let spawned = match SpawnBuilder::new(exec.token).spawn() {
        Ok(spawned) => spawned,
        Err(err) => {
            let _ = write!(&mut message, " spawn_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("spawn_ready");

    let _ = client.close_file(file);
    let _ = client.close_dir(root);

    let _ = write!(
        &mut message,
        " handles={} root_handle={} file_handle={} exec_handle={} clock_kind={} path={} exec_bytes={} child_process={} child_thread={}",
        handles.len(),
        root_handle.raw(),
        file_handle.raw(),
        exec_handle.raw(),
        clock.kind().name(),
        EXEC_PATH,
        exec.file_bytes,
        spawned.process_slot(),
        spawned.thread_slot()
    );
    abort_with(message)
}

entry_point!(main);
