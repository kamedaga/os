#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::fmt::Write as _;

use rt_alloc as _;

fn main() -> cap_std::Result<()> {
    let mut line = String::from("CapwmSmoke: ");
    let mut client = match capwm_client::Client::connect_from_registry_shadow() {
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

    let geometry_a = Geometry {
        x: 42,
        y: 54,
        width: 360,
        height: 240,
    };
    let geometry_b = Geometry {
        x: 190,
        y: 128,
        width: 300,
        height: 120,
    };

    if let Err(err) = client.set_geometry(
        window_a,
        geometry_a.x,
        geometry_a.y,
        geometry_a.width,
        geometry_a.height,
    ) {
        let _ = write!(
            &mut line,
            "set_geometry failed window={} surface={}: {:?}",
            window_a.id, window_a.surface_id, err
        );
        cap_std::println!("{}", line)?;
        return Ok(());
    }

    if let Err(err) = client.set_geometry(
        window_b,
        geometry_b.x,
        geometry_b.y,
        geometry_b.width,
        geometry_b.height,
    ) {
        let _ = write!(
            &mut line,
            "set_geometry failed window={} surface={}: {:?}",
            window_b.id, window_b.surface_id, err
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
        "ok windows=({}, {}) surfaces=({}, {}) gpu_resources=({}, {}) surfaces={}x{},{}x{} geometry=({},{} {}x{}),({},{} {}x{})",
        window_a.id,
        window_b.id,
        window_a.surface_id,
        window_b.surface_id,
        window_a.gpu_resource_id,
        window_b.gpu_resource_id,
        window_a.size.width,
        window_a.size.height,
        window_b.size.width,
        window_b.size.height,
        geometry_a.x,
        geometry_a.y,
        geometry_a.width,
        geometry_a.height,
        geometry_b.x,
        geometry_b.y,
        geometry_b.width,
        geometry_b.height
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

#[derive(Copy, Clone)]
struct Geometry {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
}

fn draw_window_surface(
    window: capwm_client::protocol::Window,
    palette: Palette,
) -> Result<(), capwm_client::Error> {
    let mut context = capwm_client::connect_gl_for_window(window)?;
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
        .map_err(|_| capwm_client::Error::GpuUnavailable)?;
    context
        .gl_draw_vertices(
            caplibgl::Primitive::TriangleStrip,
            &vertices,
            None,
            &mut scratch,
        )
        .map_err(|_| capwm_client::Error::GpuUnavailable)?;
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
