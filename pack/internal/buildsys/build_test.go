package buildsys

import (
	"path/filepath"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestKernelOutputPath(t *testing.T) {
	workspace := &config.Workspace{Root: t.TempDir(), Kernel: config.Kernel{Dir: "kernel"}}
	for _, test := range []struct {
		step string
		want string
	}{
		{"limine", filepath.Join("kernel", "zig-out", "bin", "limine", "pacha-kernel.elf")},
		{"boot-diag", filepath.Join("kernel", "zig-out", "bin", "limine", "DIAGBOOT.ELF")},
		{"check", filepath.Join("kernel", "zig-out", "bin", "check")},
	} {
		t.Run(test.step, func(t *testing.T) {
			got := KernelOutputPath(workspace, test.step)
			want := workspace.Path(test.want)
			if got != want {
				t.Fatalf("KernelOutputPath(%q) = %q, want %q", test.step, got, want)
			}
		})
	}
}

func TestSelectAppsBatch(t *testing.T) {
	workspace := &config.Workspace{AppsMap: map[string]config.App{
		"usb_core": {ID: "usb_core"},
		"usb_hid":  {ID: "usb_hid"},
	}}
	apps, err := selectApps(workspace, "", []string{"usb_hid", "usb_core"})
	if err != nil || len(apps) != 2 || apps[0].ID != "usb_hid" || apps[1].ID != "usb_core" {
		t.Fatalf("batch selection: apps=%v err=%v", apps, err)
	}
	if _, err := selectApps(workspace, "", []string{"usb_core", "usb_core"}); err == nil {
		t.Fatal("duplicate app was accepted")
	}
	if _, err := selectApps(workspace, "", []string{"missing"}); err == nil {
		t.Fatal("missing app was accepted")
	}
	if _, err := selectApps(workspace, "usb_core", []string{"usb_hid"}); err == nil {
		t.Fatal("mixed single and batch selectors were accepted")
	}
}
