use core::ptr::read_volatile;

use crate::capwm::{Position, Size};

const POINTER_SHARED_VA: usize = 0x3C00_3000;
const POINTER_SHARED_MAGIC: u64 = 0x4D53_4852;

#[repr(C)]
struct PointerSharedPage {
    magic: u64,
    width: u64,
    height: u64,
    pitch: u64,
    cursor_x: u64,
    cursor_y: u64,
    buttons: u64,
    seq: u64,
    wheel: u64,
    log_len: u64,
}

#[derive(Copy, Clone)]
pub struct PointerState {
    pub seq: u64,
    pub position: Position,
    pub size: Size,
    pub buttons: u64,
}

pub fn read() -> Option<PointerState> {
    let mut attempts = 0;
    while attempts < 8 {
        if let Some(state) = read_once() {
            return Some(state);
        }
        attempts += 1;
    }
    None
}

fn read_once() -> Option<PointerState> {
    let page = POINTER_SHARED_VA as *const PointerSharedPage;
    let magic = unsafe { read_volatile(&(*page).magic) };
    if magic != POINTER_SHARED_MAGIC {
        return None;
    }
    let seq0 = unsafe { read_volatile(&(*page).seq) };
    let width = unsafe { read_volatile(&(*page).width) };
    let height = unsafe { read_volatile(&(*page).height) };
    let cursor_x = unsafe { read_volatile(&(*page).cursor_x) };
    let cursor_y = unsafe { read_volatile(&(*page).cursor_y) };
    let buttons = unsafe { read_volatile(&(*page).buttons) };
    let seq1 = unsafe { read_volatile(&(*page).seq) };
    if seq0 != seq1 || width == 0 || height == 0 {
        return None;
    }
    Some(PointerState {
        seq: seq1,
        position: Position {
            x: clamp_to_i32(cursor_x),
            y: clamp_to_i32(cursor_y),
        },
        size: Size {
            width: clamp_to_u32(width),
            height: clamp_to_u32(height),
        },
        buttons,
    })
}

fn clamp_to_i32(value: u64) -> i32 {
    if value > i32::MAX as u64 {
        i32::MAX
    } else {
        value as i32
    }
}

fn clamp_to_u32(value: u64) -> u32 {
    if value > u32::MAX as u64 {
        u32::MAX
    } else {
        value as u32
    }
}
