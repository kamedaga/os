#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write as _;

use cap_std::fs::RootDir;
use cap_std::io::Read;
use cap_std::path::Path;
use cap_std::time::Instant;
use rt_alloc as _;

const DEMO_PATH: &str = "/sys/startup_manifest.txt";

fn sanitize_append(dst: &mut String, bytes: &[u8]) {
    for &byte in bytes {
        match byte {
            b'\r' => {}
            b'\n' => dst.push('|'),
            b'\t' => dst.push(' '),
            0x20..=0x7e => dst.push(byte as char),
            _ => dst.push('.'),
        }
    }
}

fn main() -> cap_std::Result<()> {
    let start = Instant::now();
    let root = RootDir::connect_default()?;
    let metadata = root.metadata(Path::new(DEMO_PATH))?;
    let mut file = root.open_file(Path::new(DEMO_PATH))?;

    let mut read_buf = [0u8; 96];
    let bytes_read = file.read(&mut read_buf)?;

    let mut read_dir = root.read_dir(Path::new("/sys"))?;
    let first_entry = read_dir.next().transpose()?;

    let mut message = String::from("cap std fs demo");
    let _ = write!(
        &mut message,
        " ticks={} path={} size={} mtime={} bytes_read={}",
        start.elapsed_ticks(),
        DEMO_PATH,
        metadata.len(),
        metadata.modified().as_unix_seconds(),
        bytes_read
    );
    if let Some(entry) = first_entry {
        let _ = write!(
            &mut message,
            " first_entry={} first_kind={}",
            entry.file_name().as_str(),
            entry.file_type().kind_name()
        );
    }
    message.push_str(" sample=\"");
    sanitize_append(&mut message, &read_buf[..bytes_read]);
    message.push('"');
    cap_std::println!("{}", message)?;
    Ok(())
}

cap_std::entry_point!(main);
