#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write as _;

use rt_alloc as _;

const WINDOW_WIDTH: u32 = 720;
const WINDOW_HEIGHT: u32 = 360;
const FRAME_COUNT: u32 = 96;

fn main() -> cap_std::Result<()> {
    let mut line = String::from("CapglShaderWave: ");
    let mut client = match cap_window::Client::connect_from_registry_shadow() {
        Ok(client) => client,
        Err(err) => {
            if matches!(err, cap_window::Error::MissingService) {
                return run_direct_gpu(line);
            }
            let _ = write!(&mut line, "window connect failed: {:?}", err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
    };

    if let Err(err) = client.hello() {
        let _ = write!(&mut line, "hello failed: {:?}", err);
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    let window = match client.create_window("capgl shader wave", WINDOW_WIDTH, WINDOW_HEIGHT) {
        Ok(window) => window,
        Err(err) => {
            let _ = write!(&mut line, "create_window failed: {:?}", err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
    };

    let mut context = match cap_window::connect_gl_for_window(window) {
        Ok(context) => context,
        Err(err) => {
            let _ = write!(
                &mut line,
                "gl connect failed window={} surface={}: {:?}",
                window.id, window.surface_id, err
            );
            cap_std::println!("{}", line)?;
            return Ok(());
        }
    };

    let mut scratch = [0_u8; caplibgl::FRAME_SCRATCH_BYTES];
    let mut rendered = 0;
    while rendered < FRAME_COUNT {
        let phase = rendered as f32 * 0.095;
        if let Err(err) = context.clear_color(
            caplibgl::Color {
                r: 0.015,
                g: 0.020,
                b: 0.030,
                a: 1.0,
            },
            &mut scratch,
        ) {
            let _ = write!(&mut line, "clear failed frame={}: {:?}", rendered, err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
        if let Err(err) = context.draw_loading_placeholder_preview(phase, &mut scratch) {
            let _ = write!(&mut line, "draw failed frame={}: {:?}", rendered, err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
        if let Err(err) = client.present(window) {
            let _ = write!(
                &mut line,
                "present failed frame={} window={} surface={}: {:?}",
                rendered, window.id, window.surface_id, err
            );
            cap_std::println!("{}", line)?;
            return Ok(());
        }
        wait_ticks(1);
        rendered += 1;
    }

    let _ = write!(
        &mut line,
        "ok window={} surface={} gpu_resource={} gpu_surface={} size={}x{} frames={}",
        window.id,
        window.surface_id,
        window.gpu_resource_id,
        window.gpu_surface_id,
        window.content_size.width,
        window.content_size.height,
        rendered
    );
    cap_std::println!("{}", line)?;
    Ok(())
}

cap_std::entry_point!(main);

fn run_direct_gpu(mut line: String) -> cap_std::Result<()> {
    let mut context = match caplibgl::Context::connect_from_registry_shadow() {
        Ok(context) => context,
        Err(err) => {
            let _ = write!(&mut line, "direct gpu connect failed: {:?}", err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
    };

    let target = context.surface();
    let mut scratch = [0_u8; caplibgl::FRAME_SCRATCH_BYTES];
    let mut rendered = 0;
    while rendered < FRAME_COUNT {
        let phase = rendered as f32 * 0.095;
        if let Err(err) = context.clear_color(
            caplibgl::Color {
                r: 0.015,
                g: 0.020,
                b: 0.030,
                a: 1.0,
            },
            &mut scratch,
        ) {
            let _ = write!(
                &mut line,
                "direct clear failed frame={}: {:?}",
                rendered, err
            );
            cap_std::println!("{}", line)?;
            return Ok(());
        }
        if let Err(err) = context.draw_loading_placeholder_preview(phase, &mut scratch) {
            let _ = write!(
                &mut line,
                "direct draw failed frame={}: {:?}",
                rendered, err
            );
            cap_std::println!("{}", line)?;
            return Ok(());
        }
        if let Err(err) = context.present() {
            let _ = write!(
                &mut line,
                "direct present failed frame={}: {:?}",
                rendered, err
            );
            cap_std::println!("{}", line)?;
            return Ok(());
        }
        wait_ticks(1);
        rendered += 1;
    }

    let _ = write!(
        &mut line,
        "ok mode=direct target={}x{} resource={} surface={} frames={}",
        target.width, target.height, target.resource_id, target.surface_id, rendered
    );
    cap_std::println!("{}", line)?;
    Ok(())
}

fn wait_ticks(ticks: u64) {
    let _ = rt_core::syscall::call2(rt_core::syscall::WAIT_EVENT, 0, ticks);
}
