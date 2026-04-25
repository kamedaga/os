#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::ffi::{c_char, c_void};
use core::fmt::Write;
use core::mem::size_of_val;

use rt_alloc as _;
use rt_core::entry_point;

fn main() -> ! {
    let mut message = String::from("RustGlQuery: ");

    if caplibgl::capglInit() == caplibgl::GL_TRUE {
        let mut caps = caplibgl::CapglCaps {
            features: 0,
            capset_id: 0,
            capset_max_version: 0,
            width: 0,
            height: 0,
            resource_id: 0,
            surface_id: 0,
        };
        let _ = caplibgl::capglGetCaps(&mut caps);
        let _ = write!(
            &mut message,
            "features=0x{:X} capset_id={} capset_max_version={} target={}x{} resource_id={} surface_id={}",
            caps.features,
            caps.capset_id,
            caps.capset_max_version,
            caps.width,
            caps.height,
            caps.resource_id,
            caps.surface_id
        );

        let default_context = caplibgl::capglGetCurrentContext();
        let default_surface = caplibgl::capglGetCurrentSurface();
        let context = caplibgl::capglCreateContext();
        let surface = caplibgl::capglCreateSurface(caps.width, caps.height);
        if context == 0
            || surface == 0
            || caplibgl::capglMakeCurrent(context, surface) != caplibgl::GL_TRUE
        {
            let _ = write!(
                &mut message,
                " create_surface failed gl_error=0x{:X}",
                caplibgl::glGetError()
            );
            message.push('\n');
            rt_core::log(&message);
            rt_core::abort()
        }
        let mut surface_info = caplibgl::CapglSurfaceInfo {
            width: 0,
            height: 0,
            resource_id: 0,
            surface_id: 0,
            swap_count: 0,
        };
        if caplibgl::capglGetSurfaceInfo(surface, &mut surface_info) != caplibgl::GL_TRUE {
            let _ = write!(
                &mut message,
                " surface_info failed gl_error=0x{:X}",
                caplibgl::glGetError()
            );
            message.push('\n');
            rt_core::log(&message);
            rt_core::abort()
        }

        caplibgl::glViewport(0, 0, caps.width as i32, caps.height as i32);
        let mut viewport = [0i32; 4];
        caplibgl::glGetIntegerv(caplibgl::GL_VIEWPORT, viewport.as_mut_ptr());
        caplibgl::glClearColor(0.08, 0.16, 0.28, 1.0);
        caplibgl::glClear(caplibgl::GL_COLOR_BUFFER_BIT);
        if caplibgl::glGetError() == caplibgl::GL_NO_ERROR {
            let vertex_shader_source = b"#version 100\nattribute vec4 position;\nattribute vec4 color;\nattribute vec2 texcoord;\nuniform mat4 u_mvp;\nvarying vec4 v_color;\nvarying vec2 v_texcoord;\nvoid main() { v_color = color; v_texcoord = texcoord; gl_Position = u_mvp * position; }\n\0";
            let fragment_shader_source =
                b"#version 100\nprecision mediump float;\nvarying vec4 v_color;\nvarying vec2 v_texcoord;\nuniform vec4 u_tint;\nuniform sampler2D u_tex;\nvoid main() { gl_FragColor = texture2D(u_tex, v_texcoord) * v_color * u_tint; }\n\0";
            let vertex_shader = caplibgl::glCreateShader(caplibgl::GL_VERTEX_SHADER);
            let vertex_shader_ptr = vertex_shader_source.as_ptr().cast::<c_char>();
            caplibgl::glShaderSource(
                vertex_shader,
                1,
                &vertex_shader_ptr as *const *const c_char,
                core::ptr::null(),
            );
            caplibgl::glCompileShader(vertex_shader);
            let fragment_shader = caplibgl::glCreateShader(caplibgl::GL_FRAGMENT_SHADER);
            let fragment_shader_ptr = fragment_shader_source.as_ptr().cast::<c_char>();
            caplibgl::glShaderSource(
                fragment_shader,
                1,
                &fragment_shader_ptr as *const *const c_char,
                core::ptr::null(),
            );
            caplibgl::glCompileShader(fragment_shader);
            let program = caplibgl::glCreateProgram();
            caplibgl::glAttachShader(program, vertex_shader);
            caplibgl::glAttachShader(program, fragment_shader);
            caplibgl::glLinkProgram(program);
            caplibgl::glUseProgram(program);
            let tint_name = b"u_tint\0";
            let tint_uniform =
                caplibgl::glGetUniformLocation(program, tint_name.as_ptr().cast::<c_char>());
            let tex_name = b"u_tex\0";
            let tex_uniform =
                caplibgl::glGetUniformLocation(program, tex_name.as_ptr().cast::<c_char>());
            if tint_uniform >= 0 {
                caplibgl::glUniform4f(tint_uniform, 1.0, 1.0, 1.0, 1.0);
            }
            let pixels_a = [
                255u8, 64, 64, 255, 64, 255, 64, 255, //
                64, 96, 255, 255, 255, 240, 64, 255,
            ];
            let pixels_b = [
                64u8, 255, 240, 255, 120, 120, 255, 255, //
                255, 96, 220, 255, 255, 255, 255, 255,
            ];
            let mut textures = [0u32; 3];
            caplibgl::glGenTextures(3, textures.as_mut_ptr());
            caplibgl::glActiveTexture(caplibgl::GL_TEXTURE0);
            caplibgl::glBindTexture(caplibgl::GL_TEXTURE_2D, textures[0]);
            caplibgl::glTexParameteri(
                caplibgl::GL_TEXTURE_2D,
                caplibgl::GL_TEXTURE_MIN_FILTER,
                caplibgl::GL_NEAREST as i32,
            );
            caplibgl::glTexParameteri(
                caplibgl::GL_TEXTURE_2D,
                caplibgl::GL_TEXTURE_MAG_FILTER,
                caplibgl::GL_NEAREST as i32,
            );
            caplibgl::glTexImage2D(
                caplibgl::GL_TEXTURE_2D,
                0,
                caplibgl::GL_RGBA as i32,
                2,
                2,
                0,
                caplibgl::GL_RGBA,
                caplibgl::GL_UNSIGNED_BYTE,
                pixels_a.as_ptr().cast::<c_void>(),
            );
            let patch = [255u8, 255, 255, 255];
            caplibgl::glTexSubImage2D(
                caplibgl::GL_TEXTURE_2D,
                0,
                1,
                0,
                1,
                1,
                caplibgl::GL_RGBA,
                caplibgl::GL_UNSIGNED_BYTE,
                patch.as_ptr().cast::<c_void>(),
            );
            caplibgl::glBindTexture(caplibgl::GL_TEXTURE_2D, textures[1]);
            caplibgl::glTexParameteri(
                caplibgl::GL_TEXTURE_2D,
                caplibgl::GL_TEXTURE_MIN_FILTER,
                caplibgl::GL_NEAREST as i32,
            );
            caplibgl::glTexParameteri(
                caplibgl::GL_TEXTURE_2D,
                caplibgl::GL_TEXTURE_MAG_FILTER,
                caplibgl::GL_NEAREST as i32,
            );
            caplibgl::glTexImage2D(
                caplibgl::GL_TEXTURE_2D,
                0,
                caplibgl::GL_RGBA as i32,
                2,
                2,
                0,
                caplibgl::GL_RGBA,
                caplibgl::GL_UNSIGNED_BYTE,
                pixels_b.as_ptr().cast::<c_void>(),
            );
            if tex_uniform >= 0 {
                caplibgl::glUniform1i(tex_uniform, 0);
            }
            caplibgl::glMatrixMode(caplibgl::GL_PROJECTION);
            caplibgl::glLoadIdentity();
            caplibgl::glOrtho(0.0, viewport[2] as f64, viewport[3] as f64, 0.0, -1.0, 1.0);
            caplibgl::glMatrixMode(caplibgl::GL_MODELVIEW);

            let vertices = [
                0.0f32, 0.0, 0.0, 1.0, 1.0, 0.96, 0.92, 1.0, 0.0, 0.0, //
                1.0, 0.0, 0.0, 1.0, 1.0, 0.96, 0.92, 1.0, 1.0, 0.0, //
                0.0, 1.0, 0.0, 1.0, 1.0, 0.96, 0.92, 1.0, 0.0, 1.0, //
                1.0, 1.0, 0.0, 1.0, 1.0, 0.96, 0.92, 1.0, 1.0, 1.0,
            ];
            let mut vbo = 0;
            caplibgl::glGenBuffers(1, &mut vbo);
            caplibgl::glBindBuffer(caplibgl::GL_ARRAY_BUFFER, vbo);
            caplibgl::glBufferData(
                caplibgl::GL_ARRAY_BUFFER,
                size_of_val(&vertices) as isize,
                vertices.as_ptr().cast::<c_void>(),
                caplibgl::GL_STATIC_DRAW,
            );
            let translucent_quad = [
                0.0f32, 0.0, 0.0, 1.0, 0.88, 0.95, 1.0, 0.68, 0.0, 0.0, //
                1.0, 0.0, 0.0, 1.0, 0.88, 0.95, 1.0, 0.68, 1.0, 0.0, //
                0.0, 1.0, 0.0, 1.0, 0.88, 0.95, 1.0, 0.68, 0.0, 1.0, //
                1.0, 1.0, 0.0, 1.0, 0.88, 0.95, 1.0, 0.68, 1.0, 1.0,
            ];
            caplibgl::glEnableClientState(caplibgl::GL_VERTEX_ARRAY);
            caplibgl::glVertexPointer(4, caplibgl::GL_FLOAT, 40, core::ptr::null());
            caplibgl::glEnableClientState(caplibgl::GL_COLOR_ARRAY);
            caplibgl::glColorPointer(4, caplibgl::GL_FLOAT, 40, 16 as *const c_void);
            caplibgl::glEnableClientState(caplibgl::GL_TEXTURE_COORD_ARRAY);
            caplibgl::glTexCoordPointer(2, caplibgl::GL_FLOAT, 40, 32 as *const c_void);
            caplibgl::glScissor(16, 16, caps.width as i32 - 32, caps.height as i32 - 32);
            caplibgl::glEnable(caplibgl::GL_SCISSOR_TEST);
            caplibgl::glLoadIdentity();
            caplibgl::glPushMatrix();
            caplibgl::glTranslatef(72.0, 64.0, 0.0);
            caplibgl::glScalef(264.0, 184.0, 1.0);
            caplibgl::glDrawArrays(caplibgl::GL_TRIANGLE_STRIP, 0, 4);
            caplibgl::glPopMatrix();
            caplibgl::glBufferSubData(
                caplibgl::GL_ARRAY_BUFFER,
                0,
                size_of_val(&translucent_quad) as isize,
                translucent_quad.as_ptr().cast::<c_void>(),
            );
            caplibgl::glBindTexture(caplibgl::GL_TEXTURE_2D, textures[1]);
            caplibgl::glBlendFunc(caplibgl::GL_SRC_ALPHA, caplibgl::GL_ONE_MINUS_SRC_ALPHA);
            caplibgl::glEnable(caplibgl::GL_BLEND);
            caplibgl::glPushMatrix();
            caplibgl::glTranslatef(260.0, 148.0, 0.0);
            caplibgl::glScalef(244.0, 196.0, 1.0);
            caplibgl::glDrawArrays(caplibgl::GL_TRIANGLE_STRIP, 0, 4);
            caplibgl::glPopMatrix();
            caplibgl::glBindTexture(caplibgl::GL_TEXTURE_2D, 0);
            caplibgl::glDisableClientState(caplibgl::GL_COLOR_ARRAY);
            caplibgl::glDisableClientState(caplibgl::GL_TEXTURE_COORD_ARRAY);
            caplibgl::glColor4f(0.10, 0.14, 0.18, 0.92);
            caplibgl::glPushMatrix();
            caplibgl::glTranslatef(72.0, 64.0, 0.0);
            caplibgl::glScalef(264.0, 28.0, 1.0);
            caplibgl::glDrawArrays(caplibgl::GL_TRIANGLE_STRIP, 0, 4);
            caplibgl::glPopMatrix();
            let app_swap_ok = caplibgl::capglSwapBuffers(surface) == caplibgl::GL_TRUE;
            let import_ok = app_swap_ok
                && caplibgl::capglImportTexture2D(
                    textures[2],
                    surface_info.resource_id,
                    surface_info.width,
                    surface_info.height,
                ) == caplibgl::GL_TRUE;
            let make_default_ok = import_ok
                && caplibgl::capglMakeCurrent(default_context, default_surface)
                    == caplibgl::GL_TRUE;
            if make_default_ok {
                caplibgl::glViewport(0, 0, caps.width as i32, caps.height as i32);
                caplibgl::glClearColor(0.03, 0.04, 0.06, 1.0);
                caplibgl::glClear(caplibgl::GL_COLOR_BUFFER_BIT);
                caplibgl::glMatrixMode(caplibgl::GL_PROJECTION);
                caplibgl::glLoadIdentity();
                caplibgl::glOrtho(0.0, caps.width as f64, caps.height as f64, 0.0, -1.0, 1.0);
                caplibgl::glMatrixMode(caplibgl::GL_MODELVIEW);
                caplibgl::glLoadIdentity();
                caplibgl::glEnableClientState(caplibgl::GL_COLOR_ARRAY);
                caplibgl::glEnableClientState(caplibgl::GL_TEXTURE_COORD_ARRAY);
                caplibgl::glBufferSubData(
                    caplibgl::GL_ARRAY_BUFFER,
                    0,
                    size_of_val(&vertices) as isize,
                    vertices.as_ptr().cast::<c_void>(),
                );
                caplibgl::glBindTexture(caplibgl::GL_TEXTURE_2D, textures[2]);
                caplibgl::glPushMatrix();
                caplibgl::glScalef(caps.width as f32, caps.height as f32, 1.0);
                caplibgl::glDrawArrays(caplibgl::GL_TRIANGLE_STRIP, 0, 4);
                caplibgl::glPopMatrix();
            }
            let composite_draw_error = caplibgl::glGetError();
            let default_swap_ok = make_default_ok
                && composite_draw_error == caplibgl::GL_NO_ERROR
                && caplibgl::capglSwapBuffers(default_surface) == caplibgl::GL_TRUE;
            if default_swap_ok {
                let _ = write!(
                    &mut message,
                    " viewport={}x{} draw=pushpop_strip_textured_quads solid=ok client_state=ok surface={} context={} surface_resource={} virgl_surface={} swap=ok composite=ok gl_subimage=ok",
                    viewport[2],
                    viewport[3],
                    surface,
                    context,
                    surface_info.resource_id,
                    surface_info.surface_id
                );
            } else {
                let reason = if !app_swap_ok {
                    "app_swap"
                } else if !import_ok {
                    "import"
                } else if !make_default_ok {
                    "make_default"
                } else if composite_draw_error != caplibgl::GL_NO_ERROR {
                    "composite_draw"
                } else {
                    "default_swap"
                };
                let _ = write!(
                    &mut message,
                    " present failed stage={} gl_error=0x{:X}",
                    reason,
                    if composite_draw_error != caplibgl::GL_NO_ERROR {
                        composite_draw_error
                    } else {
                        caplibgl::glGetError()
                    }
                );
            }
            caplibgl::glDeleteBuffers(1, &vbo);
            caplibgl::glDeleteTextures(3, textures.as_ptr());
        } else {
            let _ = write!(
                &mut message,
                " clear failed gl_error=0x{:X}",
                caplibgl::glGetError()
            );
        }
        let error = caplibgl::glGetError();
        if error != caplibgl::GL_NO_ERROR {
            let _ = write!(&mut message, " gl_error=0x{:X}", error);
        }
    } else {
        let _ = write!(
            &mut message,
            "capglInit failed gl_error=0x{:X}",
            caplibgl::glGetError()
        );
    }

    message.push('\n');
    rt_core::log(&message);
    rt_core::abort()
}

entry_point!(main);
