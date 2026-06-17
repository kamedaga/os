package boottest

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/qemu"
)

const deviceFDSmokeMarker = "[device_fd_boot_smoke] OK"

type DeviceFDSmokeOptions struct {
	Timeout   time.Duration
	NoKVM     bool
	ExtraArgs []string
}

type DeviceFDSmokeResult struct {
	SmokeELF  string
	KernelEFI string
	Disk      string
	Serial    string
	Log       string
	Marker    string
	Timeout   time.Duration
	Restored  bool
}

func RunDeviceFDSmoke(workspace *config.Workspace, opts DeviceFDSmokeOptions) (DeviceFDSmokeResult, error) {
	if opts.Timeout <= 0 {
		opts.Timeout = 30 * time.Second
	}
	artifactDir := workspace.Path(workspace.Artifacts, "boot-tests", "device-fd-smoke")
	if err := os.MkdirAll(artifactDir, 0o755); err != nil {
		return DeviceFDSmokeResult{}, err
	}

	smokeELF := filepath.Join(artifactDir, "INITAPP.ELF")
	if err := buildDeviceFDSmokeELF(workspace, smokeELF); err != nil {
		return DeviceFDSmokeResult{SmokeELF: smokeELF, Marker: deviceFDSmokeMarker, Timeout: opts.Timeout}, err
	}

	kernelEFI := workspace.Path(workspace.Kernel.Dir, "zig-out", "bin", "EFI", "BOOT", "BOOTX64.EFI")
	if stat, err := os.Stat(kernelEFI); err != nil {
		return DeviceFDSmokeResult{SmokeELF: smokeELF, KernelEFI: kernelEFI, Marker: deviceFDSmokeMarker, Timeout: opts.Timeout}, err
	} else if stat.Size() == 0 {
		return DeviceFDSmokeResult{SmokeELF: smokeELF, KernelEFI: kernelEFI, Marker: deviceFDSmokeMarker, Timeout: opts.Timeout}, fmt.Errorf("%s is empty", kernelEFI)
	}

	diskPath := workspace.Path(workspace.Disk.Image)
	mtoolsImage, err := espMToolsImage(workspace, diskPath)
	if err != nil {
		return DeviceFDSmokeResult{SmokeELF: smokeELF, KernelEFI: kernelEFI, Disk: diskPath, Marker: deviceFDSmokeMarker, Timeout: opts.Timeout}, err
	}

	initBackup := filepath.Join(artifactDir, "INITAPP.original.ELF")
	kernelBackup := filepath.Join(artifactDir, "BOOTX64.original.EFI")
	result := DeviceFDSmokeResult{
		SmokeELF:  smokeELF,
		KernelEFI: kernelEFI,
		Disk:      diskPath,
		Marker:    deviceFDSmokeMarker,
		Timeout:   opts.Timeout,
	}

	if err := mcopyOut(mtoolsImage, "::/EFI/BOOT/INITAPP.ELF", initBackup); err != nil {
		return result, err
	}
	if err := mcopyOut(mtoolsImage, "::/EFI/BOOT/BOOTX64.EFI", kernelBackup); err != nil {
		return result, err
	}

	restored := false
	restore := func() error {
		var errs []error
		if err := mcopyIn(mtoolsImage, initBackup, "::/EFI/BOOT/INITAPP.ELF"); err != nil {
			errs = append(errs, err)
		}
		if err := mcopyIn(mtoolsImage, kernelBackup, "::/EFI/BOOT/BOOTX64.EFI"); err != nil {
			errs = append(errs, err)
		}
		if len(errs) == 0 {
			restored = true
			return nil
		}
		return errors.Join(errs...)
	}
	if err := mcopyIn(mtoolsImage, smokeELF, "::/EFI/BOOT/INITAPP.ELF"); err != nil {
		restoreErr := restore()
		result.Restored = restored
		return result, errors.Join(err, restoreErr)
	}
	if err := mcopyIn(mtoolsImage, kernelEFI, "::/EFI/BOOT/BOOTX64.EFI"); err != nil {
		restoreErr := restore()
		result.Restored = restored
		return result, errors.Join(err, restoreErr)
	}

	smoke, smokeErr := qemu.Smoke(workspace, qemu.SmokeOptions{
		Timeout:   opts.Timeout,
		NoKVM:     opts.NoKVM,
		NoNet:     true,
		ExtraArgs: opts.ExtraArgs,
		Marker:    deviceFDSmokeMarker,
	})
	result.Serial = smoke.Serial
	result.Log = smoke.Log

	restoreErr := restore()
	result.Restored = restored
	return result, errors.Join(smokeErr, restoreErr)
}

func buildDeviceFDSmokeELF(workspace *config.Workspace, out string) error {
	clang := os.Getenv("CAPOS_CLANG")
	if clang == "" {
		clang = "clang"
	}
	srcDir := workspace.Path("tests", "boot", "device_fd_smoke")
	libcapsuleDir := workspace.Path("userland", "libcapsule")
	libpachaDir := workspace.Path("userland", "libpacha")
	args := []string{
		"-target", "x86_64-freestanding-none",
		"-ffreestanding",
		"-fno-stack-protector",
		"-fno-plt",
		"-fPIE",
		"-mno-red-zone",
		"-O2",
		"-Wall",
		"-Wextra",
		"-nostdlib",
		"-fuse-ld=lld",
		"-Wl,-e,_start",
		"-Wl,-pie",
		"-Wl,--no-dynamic-linker",
		"-Wl,-z,common-page-size=4096",
		"-Wl,-z,max-page-size=4096",
		"-I", filepath.Join(libcapsuleDir, "include"),
		"-I", filepath.Join(libpachaDir, "include"),
		filepath.Join(srcDir, "entry.S"),
		filepath.Join(srcDir, "main.c"),
		filepath.Join(libcapsuleDir, "src", "capsule.c"),
		filepath.Join(libpachaDir, "src", "syscall.c"),
		"-o", out,
	}
	cmd := exec.Command(clang, args...)
	cmd.Dir = workspace.Root
	if output, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("%s failed: %w\n%s", clang, err, string(output))
	}
	return nil
}
