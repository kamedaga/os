#!/usr/bin/env python3
"""Send diagnostic keystrokes only while a verified QEMU window has focus."""
import argparse
import ctypes as c
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--pid', type=int, required=True)
p.add_argument('--window', type=lambda s: int(s, 0), required=True)
action = p.add_mutually_exclusive_group(required=True)
action.add_argument('--chord')
action.add_argument('--text')
a = p.parse_args()
x = c.CDLL('libX11.so.6')
xt = c.CDLL('libXtst.so.6')
P, U, I = c.c_void_p, c.c_ulong, c.c_int
def bind(lib, name, result, args):
    f = getattr(lib, name)
    f.restype, f.argtypes = result, args
    return f
bind(x, 'XOpenDisplay', P, [c.c_char_p])
bind(x, 'XInternAtom', U, [P, c.c_char_p, I])
bind(x, 'XGetWindowProperty', I, [P,U,U,c.c_long,c.c_long,I,U,c.POINTER(U),c.POINTER(I),c.POINTER(U),c.POINTER(U),c.POINTER(P)])
bind(x, 'XFree', I, [P])
bind(x, 'XGetInputFocus', I, [P,c.POINTER(U),c.POINTER(I)])
bind(x, 'XQueryTree', I, [P,U,c.POINTER(U),c.POINTER(U),c.POINTER(P),c.POINTER(c.c_uint)])
bind(x, 'XSetInputFocus', I, [P,U,I,U])
bind(x, 'XRaiseWindow', I, [P,U])
bind(x, 'XStringToKeysym', U, [c.c_char_p])
bind(x, 'XKeysymToKeycode', c.c_ubyte, [P,U])
bind(x, 'XSync', I, [P,I])
bind(x, 'XCloseDisplay', I, [P])
bind(xt, 'XTestFakeKeyEvent', I, [P,c.c_uint,I,U])
d = x.XOpenDisplay(None)
if not d: raise SystemExit('No display')
actual, count, remaining, fmt, value = U(), U(), U(), I(), P()
status = x.XGetWindowProperty(d,a.window,x.XInternAtom(d,b'_NET_WM_PID',0),0,1,0,6,c.byref(actual),c.byref(fmt),c.byref(count),c.byref(remaining),c.byref(value))
matches = not status and actual.value == 6 and fmt.value == 32 and count.value == 1 and value.value and c.cast(value,c.POINTER(U))[0] == a.pid
if value: x.XFree(value)
if not matches: raise SystemExit('QEMU window/PID mismatch')

def focused():
    w, revert = U(), I()
    x.XGetInputFocus(d,c.byref(w),c.byref(revert))
    for _ in range(32):
        if w.value == a.window: return True
        if w.value <= 1: return False
        root, parent, children, n = U(), U(), P(), c.c_uint()
        if not x.XQueryTree(d,w,c.byref(root),c.byref(parent),c.byref(children),c.byref(n)): return False
        if children: x.XFree(children)
        w = parent
    return False

def chord(names):
    if not focused(): raise RuntimeError('Focus changed; stopped before sending more input')
    codes = [x.XKeysymToKeycode(d,x.XStringToKeysym(n.encode())) for n in names]
    if not all(codes): raise ValueError(names)
    pressed = []
    try:
        for code in codes:
            if not focused(): raise RuntimeError('Focus changed during chord')
            xt.XTestFakeKeyEvent(d,code,1,0)
            pressed.append(code)
            x.XSync(d,0)
        time.sleep(.07)
    finally:
        for code in reversed(pressed): xt.XTestFakeKeyEvent(d,code,0,0)
        x.XSync(d,0)
    time.sleep(.05)

x.XRaiseWindow(d,a.window)
x.XSetInputFocus(d,a.window,1,0)
x.XSync(d,0)
time.sleep(.15)
try:
    if a.chord:
        chord(a.chord.split('+'))
    else:
        plain = {' ':'space','\n':'Return','-':'minus','/':'slash','.':'period','=':'equal',"'":'apostrophe'}
        shifted = {':':'semicolon','_':'minus','>':'period','&':'7','|':'backslash','"':'apostrophe','?':'slash'}
        for char in a.text:
            if char in shifted: chord(['Shift_L',shifted[char]])
            elif char.isascii() and char.isupper(): chord(['Shift_L',char.lower()])
            elif char in plain: chord([plain[char]])
            elif char.isascii() and char.isalnum(): chord([char])
            else: raise ValueError(f'Unsupported character {char!r}')
    print('QEMU_KEYS_SENT')
finally:
    x.XCloseDisplay(d)
