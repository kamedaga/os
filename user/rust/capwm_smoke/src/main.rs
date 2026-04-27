#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write as _;

use rt_alloc as _;

fn main() -> cap_std::Result<()> {
    let mut line = String::from("CapwmSmoke: ");
    let mut client = match cap_window::Client::connect_from_registry_shadow() {
        Ok(client) => client,
        Err(err) => {
            let _ = write!(&mut line, "connect failed: {:?}", err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
    };

    if let Err(err) = client.hello() {
        let _ = write!(&mut line, "hello failed: {:?}", err);
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    let window_a = match client.create_window("capwm-smoke-a", 320, 180) {
        Ok(window) => window,
        Err(err) => {
            let _ = write!(&mut line, "create_window failed: {:?}", err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
    };

    let window_b = match client.create_window("capwm-smoke-b", 280, 170) {
        Ok(window) => window,
        Err(err) => {
            let _ = write!(&mut line, "create_window failed: {:?}", err);
            cap_std::println!("{}", line)?;
            return Ok(());
        }
    };

    if let Err(err) = draw_window_surface(window_a, Palette::Warm) {
        let _ = write!(
            &mut line,
            "draw failed window={} surface={} gpu_resource={} gpu_surface={}: {:?}",
            window_a.id,
            window_a.surface_id,
            window_a.gpu_resource_id,
            window_a.gpu_surface_id,
            err
        );
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    if let Err(err) = draw_window_surface(window_b, Palette::Cool) {
        let _ = write!(
            &mut line,
            "draw failed window={} surface={} gpu_resource={} gpu_surface={}: {:?}",
            window_b.id,
            window_b.surface_id,
            window_b.gpu_resource_id,
            window_b.gpu_surface_id,
            err
        );
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    if let Err(err) = client.present(window_a) {
        let _ = write!(
            &mut line,
            "present failed window={} surface={}: {:?}",
            window_a.id, window_a.surface_id, err
        );
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    if let Err(err) = client.present(window_b) {
        let _ = write!(
            &mut line,
            "present failed window={} surface={}: {:?}",
            window_b.id, window_b.surface_id, err
        );
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    if let Err(err) = client.destroy_window(window_a.id) {
        let _ = write!(
            &mut line,
            "destroy_window failed window={} surface={}: {:?}",
            window_a.id, window_a.surface_id, err
        );
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    if let Err(err) = client.destroy_window(window_b.id) {
        let _ = write!(
            &mut line,
            "destroy_window failed window={} surface={}: {:?}",
            window_b.id, window_b.surface_id, err
        );
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    let _ = write!(
        &mut line,
        "ok windows=({}, {}) surfaces=({}, {}) gpu_resources=({}, {}) content={}x{},{}x{}",
        window_a.id,
        window_b.id,
        window_a.surface_id,
        window_b.surface_id,
        window_a.gpu_resource_id,
        window_b.gpu_resource_id,
        window_a.content_size.width,
        window_a.content_size.height,
        window_b.content_size.width,
        window_b.content_size.height
    );
    cap_std::println!("{}", line)?;
    Ok(())
}

cap_std::entry_point!(main);

#[derive(Copy, Clone)]
enum Palette {
    Warm,
    Cool,
}

fn draw_window_surface(
    window: cap_window::protocol::Window,
    palette: Palette,
) -> Result<(), cap_window::Error> {
    let mut context = cap_window::connect_gl_for_window(window)?;
    let mut scratch = [0_u8; caplibgl::FRAME_SCRATCH_BYTES];
    let (top_left, bottom_left, top_right, bottom_right) = palette.colors();
    let vertices = [
        vertex(-1.0, 1.0, top_left),
        vertex(-1.0, -1.0, bottom_left),
        vertex(1.0, 1.0, top_right),
        vertex(1.0, -1.0, bottom_right),
    ];
    context
        .clear_color(
            caplibgl::Color {
                r: 0.02,
                g: 0.025,
                b: 0.035,
                a: 1.0,
            },
            &mut scratch,
        )
        .map_err(|_| cap_window::Error::GpuUnavailable)?;
    context
        .gl_draw_vertices(
            caplibgl::Primitive::TriangleStrip,
            &vertices,
            None,
            &mut scratch,
        )
        .map_err(|_| cap_window::Error::GpuUnavailable)?;
    Ok(())
}

impl Palette {
    const fn colors(self) -> ([f32; 4], [f32; 4], [f32; 4], [f32; 4]) {
        match self {
            Self::Warm => (
                [0.95, 0.15, 0.35, 0.94],
                [0.95, 0.72, 0.15, 0.88],
                [0.95, 0.92, 0.25, 0.90],
                [0.88, 0.20, 0.28, 0.84],
            ),
            Self::Cool => (
                [0.20, 0.75, 0.95, 0.94],
                [0.14, 0.32, 0.92, 0.88],
                [0.18, 0.92, 0.68, 0.90],
                [0.45, 0.22, 0.92, 0.84],
            ),
        }
    }
}

fn vertex(x: f32, y: f32, rgba: [f32; 4]) -> caplibgl::Vertex {
    caplibgl::Vertex {
        x,
        y,
        z: 0.0,
        w: 1.0,
        r: rgba[0],
        g: rgba[1],
        b: rgba[2],
        a: rgba[3],
        u: 0.0,
        v: 0.0,
        s: 0.0,
        t: 1.0,
    }
}
