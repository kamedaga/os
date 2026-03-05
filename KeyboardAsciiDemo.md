# Keyboard ASCII Demo (Two Process Display)

## Goal
- Keep two user processes rendering concurrently on the shared virtual framebuffer.
- Add a keyboard process that shows pressed keys as ASCII.
- Keep the keyboard driver as input-only (no framebuffer draw in driver).

## Process Roles
- `Process2` (`mouse_button_demo`): mouse-only UI renderer:
  - mouse button state on the **left half**
- `Process3` (`keyboard_driver`): input-only keyboard driver.
- `Process4` (`keyboard_ascii_demo`): keyboard-only UI renderer:
  - pressed-key ASCII history on the **right half**
- `Process1` (`compositor`): composes shared VFB onto the real framebuffer.

## Kernel Changes
- Added keyboard shared page VA:
  - driver side: `0x3C00_6000`
  - renderer side: `0x3C00_6000`
- Keyboard config page now includes `keyboard_shared_paddr` at `word[10]`.
- Added `Process4/Thread4` for keyboard UI process.
- In mouse/compositor modes, kernel grants keyboard shared capability from `Process3` to `Process4`.
- Virtual framebuffer capability/mapping now includes `Process4` when keyboard demo is enabled.
- If keyboard is absent, kernel maps fallback keyboard-shared page to `Process4` (no crash).

## User Program Changes
- `mouse_button_demo.zig`
  - renderer process for mouse UI only
  - reads mouse shared page only
  - draws left panel mouse state
- `keyboard_ascii_demo.zig`
  - renderer process for keyboard UI only
  - reads keyboard shared page
  - draws right panel ASCII history
- `keyboard_driver.zig`
  - reads key events and converts Linux input key code to ASCII
  - writes latest key event to keyboard shared page (`seq/ascii/code/value`)
  - no framebuffer drawing
  - keeps existing `key code=... value=...` userlog output

## Expected Behavior
- After boot and compositor launch:
  - left panel reflects mouse button presses (`Process2` render)
  - right panel shows pressed-key ASCII history (`Process4` render)
  - `Process3` only updates shared page via IPC
- Mouse and keyboard UI are separated by process/file responsibility.
