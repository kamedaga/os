import ctypes as c
import struct
import time
import zlib
import os
import re
import threading
from pathlib import Path

x = c.CDLL('libX11.so.6')
display_t, window_t = c.c_void_p, c.c_ulong
x.XOpenDisplay.argtypes = [c.c_char_p]
x.XOpenDisplay.restype = display_t
x.XQueryTree.argtypes = [display_t, window_t, c.POINTER(window_t), c.POINTER(window_t), c.POINTER(c.POINTER(window_t)), c.POINTER(c.c_uint)]
x.XFetchName.argtypes = [display_t, window_t, c.POINTER(c.c_char_p)]
x.XDefaultRootWindow.argtypes = [display_t]
x.XDefaultRootWindow.restype = window_t
x.XGetGeometry.argtypes = [display_t, window_t, c.POINTER(window_t), c.POINTER(c.c_int), c.POINTER(c.c_int), c.POINTER(c.c_uint), c.POINTER(c.c_uint), c.POINTER(c.c_uint), c.POINTER(c.c_uint)]
x.XGetImage.argtypes = [display_t, window_t, c.c_int, c.c_int, c.c_uint, c.c_uint, c.c_ulong, c.c_int]
x.XGetImage.restype = c.c_void_p
x.XGetPixel.argtypes = [c.c_void_p, c.c_int, c.c_int]
x.XGetPixel.restype = c.c_ulong
x.XDestroyImage.argtypes = [c.c_void_p]
x.XFree.argtypes = [c.c_void_p]
d = x.XOpenDisplay(None)
assert d
root = x.XDefaultRootWindow(d)
start = time.monotonic()
window = None
while time.monotonic() - start < 90:
    children = c.POINTER(window_t)()
    n, r, parent = c.c_uint(), window_t(), window_t()
    pending = [root]
    while pending and not window:
        current = pending.pop()
        name = c.c_char_p()
        if x.XFetchName(d, current, c.byref(name)) and name.value:
            if b'QEMU' in name.value:
                window = current
            x.XFree(name)
        x.XQueryTree(d, current, c.byref(r), c.byref(parent), c.byref(children), c.byref(n))
        pending.extend(children[i] for i in range(n.value))
        if children:
            x.XFree(children)
    if window:
        break
    time.sleep(.2)
assert window, 'QEMU X11 window not found'
start = time.monotonic()
print('WINDOW', hex(window), 'epoch', time.time(), flush=True)

# Anchor to freshly received Go elapsed-time records, not the adjustable wall
# clock. Arrival delay bounds the approximation; preserve anchors for review.
anchor = None
def follow_host_log():
    global anchor
    path = Path(os.environ.get('CAPTURE_HOST_LOG',
                               '/home/kamer/os/.artifacts/qemu-tty-host-time.log'))
    if not path.exists():
        return
    with path.open() as stream:
        # An old run's last record is not a newly received clock anchor.
        # Without a live producer, leave host_elapsed/anchor_age unset.
        stream.seek(0, 2)
        while True:
            lines = stream.readlines()
            now = time.monotonic()
            for line in reversed(lines):
                match = re.search(r' \+(?:(\d+)m)?([\d.]+)(ms|s) ', line)
                if match:
                    seconds = 60 * int(match[1] or 0) + float(match[2]) / (1000 if match[3] == 'ms' else 1)
                    anchor = (now, seconds)
                    break
            time.sleep(.01)
threading.Thread(target=follow_host_log, daemon=True).start()

class XImage(c.Structure):
    _fields_ = [('width', c.c_int), ('height', c.c_int), ('xoffset', c.c_int),
        ('format', c.c_int), ('data', c.c_void_p), ('byte_order', c.c_int),
        ('bitmap_unit', c.c_int), ('bitmap_bit_order', c.c_int),
        ('bitmap_pad', c.c_int), ('depth', c.c_int), ('bytes_per_line', c.c_int),
        ('bits_per_pixel', c.c_int), ('red_mask', c.c_ulong),
        ('green_mask', c.c_ulong), ('blue_mask', c.c_ulong)]

def chunk(kind, body):
    return struct.pack('!I', len(body)) + kind + body + struct.pack('!I', zlib.crc32(kind + body))

for target in map(float, os.environ.get('CAPTURE_SECONDS', '0,20,40,60,80').split(',')):
    time.sleep(max(0, start + target - time.monotonic()))
    rx, ry, w, h, border, depth = c.c_int(), c.c_int(), c.c_uint(), c.c_uint(), c.c_uint(), c.c_uint()
    x.XGetGeometry(d, window, c.byref(r), c.byref(rx), c.byref(ry), c.byref(w), c.byref(h), c.byref(border), c.byref(depth))
    capture_start = time.monotonic()
    basis = anchor
    capture = x.XGetImage(d, window, 0, 0, w.value, h.value, c.c_ulong(-1).value, 2)
    assert capture
    captured = c.cast(capture, c.POINTER(XImage)).contents
    assert captured.bits_per_pixel == 32 and captured.byte_order == 0
    assert (captured.red_mask, captured.green_mask, captured.blue_mask) == (0xff0000, 0xff00, 0xff)
    pixels = c.string_at(captured.data, captured.bytes_per_line * h.value)
    rows = bytearray()
    for py in range(h.value):
        rows.append(0)
        row = pixels[py * captured.bytes_per_line:py * captured.bytes_per_line + w.value * 4]
        rgb = bytearray(w.value * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows.extend(rgb)
    x.XDestroyImage(capture)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', w.value, h.value, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
    (Path(os.environ.get('CAPTURE_DIR', str(Path(__file__).parent))) / f'screen-{target}.png').write_bytes(png)
    host_elapsed = capture_start - basis[0] + basis[1] if basis else None
    print('SCREEN', target, 'host_elapsed', host_elapsed, 'capture_seconds', time.monotonic()-capture_start,
          'anchor_age', capture_start-basis[0] if basis else None, 'size', w.value, h.value, flush=True)
