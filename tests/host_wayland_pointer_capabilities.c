/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data; (void)registry; (void)name;
    /* Only inspect advertised capabilities: binding a seat or grabbing a
     * pointer would disturb the desktop whose input we are diagnosing. */
    if (strstr(interface, "pointer") || !strcmp(interface, "wl_seat"))
        printf("HOST_WAYLAND_GLOBAL interface=%s version=%u\n", interface, version);
}

static void removed(void *data, struct wl_registry *registry, uint32_t name)
{ (void)data; (void)registry; (void)name; }

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fputs("Wayland connection failed\n", stderr); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener listener = { global, removed };
    int status = wl_registry_add_listener(registry, &listener, NULL);
    if (!status && wl_display_roundtrip(display) < 0) status = -1;
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return status ? 1 : 0;
}
