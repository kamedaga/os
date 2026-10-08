#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the live shell's production initializer across readiness states."""

from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]


class LiveShellClients(unittest.TestCase):
    def test_registered_endpoints_survive_desktop_import_and_dhcp(self):
        source = (REPO / "userland/seed0root/src/main.c").read_text()
        live = source[source.index("uint64_t live_linux_clients = 0;"):]
        self.assertIn("&usb_ready, &live_linux_clients);", live)
        start = live.index("const seed0root_linux_service_t shell = {")
        initializer = live[start:live.index("\n        };", start) + len("\n        };")]
        program = r'''
#include <assert.h>
#include <stdint.h>
#define SEED0ROOT_FILE_NAMESPACE 0
#define FILED_EXEC_SERVICE_TERMD 1
#define FILED_EXEC_SERVICE_NETD 2
#define FILED_EXEC_SERVICE_INPUTD 4
#define FILED_EXEC_SERVICE_GPUD_DRM 8
typedef struct {
    const char *account;
    uint64_t filed_rights, clients;
    const char *ctty;
    unsigned argc;
    const char *argv[2];
} seed0root_linux_service_t;
static const char *live_console_ctty(void *console) { (void)console; return "ttyv0"; }
int main(void) {
    void *console = 0;
    for (unsigned net_state = 0; net_state <= 3; ++net_state) {
        (void)net_state;
        for (uint64_t registered = 0; registered < 8; ++registered) {
            uint64_t live_linux_clients = registered;
            for (int gpu_probe_ready_fd = -1; gpu_probe_ready_fd <= 16; ++gpu_probe_ready_fd) {
''' + initializer + r'''
                assert(shell.clients == (registered |
                    (gpu_probe_ready_fd >= 16 ? FILED_EXEC_SERVICE_GPUD_DRM : 0)));
            }
        }
    }
}
'''
        # Extract only the real initializer so regression tests need neither
        # a kernel nor an alternative boot path to observe capability policy.
        with tempfile.TemporaryDirectory(prefix="live-shell-clients-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(program)
            subprocess.run(["/usr/bin/clang", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
