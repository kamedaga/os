#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write as _;

use rt_alloc as _;

mod capwm;
mod compositor;
mod pointer;
mod server;
mod service;
mod virgl;

fn main() -> cap_std::Result<()> {
    let mut server = server::Server::new();

    let virgl_header = virgl::command_header(
        virgl::Command::BindObject,
        virgl::ObjectKind::Surface,
        virgl::BIND_OBJECT_WORDS,
    );

    let target_size = server.target_size();
    let (target_width, target_height) = target_size
        .map(|size| (size.width, size.height))
        .unwrap_or((0, 0));

    let mut line = String::from("pachaland: ");
    let _ = write!(
        &mut line,
        "name={} internal={} protocol={} windows={} target={}x{} gl={} features=0x{:X} capset={}:{} requests={} virgl_commands={} virgl_objects={} virgl_header=0x{:X}",
        capwm::PUBLIC_NAME,
        capwm::INTERNAL_SERVICE_NAME,
        capwm::PROTOCOL_VERSION,
        server.window_count(),
        target_width,
        target_height,
        server.gpu_status_label(),
        server.gpu_features(),
        server.gpu_capset_id(),
        server.gpu_capset_max_version(),
        server::SUPPORTED_REQUESTS.len(),
        virgl::SUPPORTED_COMMANDS.len(),
        virgl::SUPPORTED_OBJECT_KINDS.len(),
        virgl_header,
    );
    cap_std::println!("{}", line)?;

    let mut host = service::ServiceHost::new(server);
    host.run_forever()
}

cap_std::entry_point!(main);
