#![no_std]

use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;
use core::ptr::null_mut;

use rt_core::syscall;

const HEAP_BASE_VA: usize = 0x3000_0000;
const PAGE_SIZE: usize = 4096;
const MAP_PAGE_WRITABLE: u64 = 1;
const SYSCALL_ERR_EMPTY: u64 = 13;

struct AllocState {
    next: usize,
    end: usize,
    next_map_va: usize,
}

struct BumpAllocator {
    state: UnsafeCell<AllocState>,
}

unsafe impl Sync for BumpAllocator {}

#[global_allocator]
static GLOBAL_ALLOCATOR: BumpAllocator = BumpAllocator {
    state: UnsafeCell::new(AllocState {
        next: HEAP_BASE_VA,
        end: HEAP_BASE_VA,
        next_map_va: HEAP_BASE_VA,
    }),
};

fn align_up(value: usize, align: usize) -> Option<usize> {
    let mask = align.checked_sub(1)?;
    value.checked_add(mask).map(|rounded| rounded & !mask)
}

fn syscall_is_error(value: u64) -> bool {
    value != 0 && value <= SYSCALL_ERR_EMPTY
}

impl BumpAllocator {
    fn map_until_state(state: &mut AllocState, end_addr: usize) -> bool {
        while end_addr > state.end {
            let page_paddr = syscall::call0(syscall::ALLOC_PAGE);
            if syscall_is_error(page_paddr) {
                return false;
            }

            let status = syscall::call3(
                syscall::MAP_PAGE,
                state.next_map_va as u64,
                page_paddr,
                MAP_PAGE_WRITABLE,
            );
            if status != syscall::OK {
                return false;
            }

            state.next_map_va += PAGE_SIZE;
            state.end += PAGE_SIZE;
        }
        true
    }
}

unsafe impl GlobalAlloc for BumpAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let state = unsafe { &mut *self.state.get() };
        let size = layout.size().max(1);
        let aligned_start = match align_up(state.next, layout.align()) {
            Some(value) => value,
            None => return null_mut(),
        };
        let end_addr = match aligned_start.checked_add(size) {
            Some(value) => value,
            None => return null_mut(),
        };
        if !Self::map_until_state(state, end_addr) {
            return null_mut();
        }
        state.next = end_addr;
        aligned_start as *mut u8
    }

    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: Layout) {
        // Phase 2 keeps allocation single-owner and monotonic.
    }
}
