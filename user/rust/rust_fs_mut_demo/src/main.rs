#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write;
use core::str;

use rt_alloc as _;
use rt_core::entry_point;
use rt_handle::{FsConnectionId, HandleTable};
use rt_io::{PersistentFsClient, ReaddirResult, monotonic_now_ticks};

const DEMO_DIR_PATH: &str = "/tmp/rt_io_demo_dir";
const DEMO_FILE_NAME: &str = "notes.txt";
const DEMO_RENAMED_NAME: &str = "notes-renamed.txt";
const DEMO_PAYLOAD: &[u8] = b"mutable rt_io demo payload";

fn abort_with(mut message: String) -> ! {
    message.push('\n');
    rt_core::log(&message);
    rt_core::abort()
}

fn log_stage(stage: &str) {
    let mut message = String::from("rust fs mut demo stage=");
    message.push_str(stage);
    message.push('\n');
    rt_core::log(&message);
}

fn cleanup_demo(client: &mut PersistentFsClient, root: rt_io::Dir) {
    let _ = client.unlink(root, "/tmp/rt_io_demo_dir/notes.txt");
    let _ = client.unlink(root, "/tmp/rt_io_demo_dir/notes-renamed.txt");
    let _ = client.unlink(root, DEMO_DIR_PATH);
}

fn main() -> ! {
    log_stage("start");
    let mut message = String::from("rust fs mut demo");
    let _ = write!(&mut message, " tick={}", monotonic_now_ticks());

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

    cleanup_demo(&mut client, root);

    let demo_dir = match client.create_dir(root, DEMO_DIR_PATH) {
        Ok(dir) => dir,
        Err(err) => {
            let _ = write!(&mut message, " create_dir_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("dir_ready");

    let file = match client.create_file(demo_dir, DEMO_FILE_NAME) {
        Ok(file) => file,
        Err(err) => {
            let _ = write!(&mut message, " create_file_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("file_ready");

    let mut handles = HandleTable::new();
    let root_handle =
        handles.insert_dir(client.connection_id(), root.token(), root.assumed_rights());
    let demo_dir_handle = handles.insert_dir(
        client.connection_id(),
        demo_dir.token(),
        demo_dir.assumed_rights(),
    );
    let file_handle =
        handles.insert_vnode_file(client.connection_id(), file.token(), file.assumed_rights());

    let open_file = match client.open_file(file) {
        Ok(open_file) => open_file,
        Err(err) => {
            let _ = write!(&mut message, " open_failed={err:?}");
            abort_with(message);
        }
    };
    let open_handle = handles.insert_open_file(
        client.connection_id(),
        open_file.token(),
        open_file.assumed_rights(),
    );
    log_stage("open_ready");

    let file_size = match client.write(open_file, 0, DEMO_PAYLOAD) {
        Ok(file_size) => file_size,
        Err(err) => {
            let _ = write!(&mut message, " write_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("write_ready");

    let stat = match client.stat_file(file) {
        Ok(stat) => stat,
        Err(err) => {
            let _ = write!(&mut message, " stat_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("stat_ready");

    let mut read_buf = [0u8; 96];
    let read = match client.read(open_file, 0, &mut read_buf) {
        Ok(read) => read,
        Err(err) => {
            let _ = write!(&mut message, " read_failed={err:?}");
            abort_with(message);
        }
    };
    let sample = match str::from_utf8(&read_buf[..read.bytes_read]) {
        Ok(sample) => sample,
        Err(_) => "<non-utf8>",
    };
    log_stage("read_ready");

    let mut dirent_buf = [0u8; 48];
    let dirent = match client.readdir_one(demo_dir, 0, &mut dirent_buf) {
        Ok(ReaddirResult::Entry(entry)) => entry,
        Ok(ReaddirResult::End) => {
            message.push_str(" readdir_failed=end");
            abort_with(message);
        }
        Err(err) => {
            let _ = write!(&mut message, " readdir_failed={err:?}");
            abort_with(message);
        }
    };
    let dirent_next_cursor = dirent.next_cursor;
    let dirent_kind = dirent.object_kind;
    let dirent_name = String::from(str::from_utf8(dirent.name).unwrap_or("<non-utf8>"));
    let readdir_end = match client.readdir_one(demo_dir, dirent_next_cursor, &mut dirent_buf) {
        Ok(ReaddirResult::End) => true,
        Ok(ReaddirResult::Entry(_)) => false,
        Err(err) => {
            let _ = write!(&mut message, " readdir_end_failed={err:?}");
            abort_with(message);
        }
    };
    log_stage("readdir_ready");

    if let Err(err) = client.close_open_file(open_file) {
        let _ = write!(&mut message, " close_open_failed={err:?}");
        abort_with(message);
    }

    if let Err(err) = client.rename(demo_dir, DEMO_FILE_NAME, DEMO_RENAMED_NAME) {
        let _ = write!(&mut message, " rename_failed={err:?}");
        abort_with(message);
    }
    log_stage("rename_ready");

    let renamed = match client.lookup_file(demo_dir, DEMO_RENAMED_NAME) {
        Ok(file) => file,
        Err(err) => {
            let _ = write!(&mut message, " lookup_renamed_failed={err:?}");
            abort_with(message);
        }
    };
    let renamed_is_same_token = renamed.token().raw() == file.token().raw();

    if let Err(err) = client.close_file(file) {
        let _ = write!(&mut message, " close_file_failed={err:?}");
        abort_with(message);
    }
    if !renamed_is_same_token {
        if let Err(err) = client.close_file(renamed) {
            let _ = write!(&mut message, " close_renamed_failed={err:?}");
            abort_with(message);
        }
    }

    if let Err(err) = client.unlink(demo_dir, DEMO_RENAMED_NAME) {
        let _ = write!(&mut message, " unlink_file_failed={err:?}");
        abort_with(message);
    }
    if let Err(err) = client.close_dir(demo_dir) {
        let _ = write!(&mut message, " close_dir_failed={err:?}");
        abort_with(message);
    }
    if let Err(err) = client.unlink(root, DEMO_DIR_PATH) {
        let _ = write!(&mut message, " unlink_dir_failed={err:?}");
        abort_with(message);
    }
    if let Err(err) = client.close_dir(root) {
        let _ = write!(&mut message, " close_root_failed={err:?}");
        abort_with(message);
    }
    log_stage("unlink_ready");

    let _ = write!(
        &mut message,
        " handles={} root_handle={} dir_handle={} file_handle={} open_handle={} size={} stat_size={} bytes_read={} next_offset={} dirent={} dirent_kind={} readdir_end={} renamed_same_token={} sample=\"{}\"",
        handles.len(),
        root_handle.raw(),
        demo_dir_handle.raw(),
        file_handle.raw(),
        open_handle.raw(),
        file_size,
        stat.size_bytes,
        read.bytes_read,
        read.next_offset,
        dirent_name,
        dirent_kind.name(),
        readdir_end,
        renamed_is_same_token,
        sample
    );
    abort_with(message)
}

entry_point!(main);
