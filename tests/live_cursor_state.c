/* SPDX-License-Identifier: MIT */
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "center"))) return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    if (argc == 2) {
        /* A single known pointer position separates cursor feedback from
         * physical USB motion; no buttons or keyboard input are generated. */
        XWarpPointer(d, None, DefaultRootWindow(d), 0, 0, 0, 0,
            DisplayWidth(d, DefaultScreen(d)) / 2,
            DisplayHeight(d, DefaultScreen(d)) / 2);
        XSync(d, False);
    }
    XFixesCursorImage *cursor = XFixesGetCursorImage(d);
    if (!cursor) { XCloseDisplay(d); return 1; }
    printf("CURSOR_STATE x=%d y=%d width=%u height=%u hot=%u,%u serial=%lu name=%s\n",
        cursor->x, cursor->y, cursor->width, cursor->height,
        cursor->xhot, cursor->yhot, cursor->cursor_serial,
        cursor->name ? cursor->name : "-");
    XFree(cursor);
    XCloseDisplay(d);
    return 0;
}
