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

const seed0BootMarker = "[seed0boot] ready"

type Seed0BootOptions struct {
	Timeout   time.Duration
	NoKVM     bool
	ExtraArgs []string
	Marker    string
	BuildInit bool
}

type Seed0BootResult struct {
	SeedELF   string
	KernelEFI string
	Disk      string
	Serial    string
	Log       string
	Marker    string
	Timeout   time.Duration
	Restored  bool
}

func RunSeed0Boot(workspace *config.Workspace, opts Seed0BootOptions) (Seed0BootResult, error) {
	if opts.Timeout <= 0 {
		opts.Timeout = 30 * time.Second
	}
	marker := opts.Marker
	if marker == "" {
		marker = seed0BootMarker
	}
	artifactDir := workspace.Path(workspace.Artifacts, "boot-tests", "seed0boot")
	if err := os.MkdirAll(artifactDir, 0o755); err != nil {
		return Seed0BootResult{}, err
	}

	seedELF := filepath.Join(artifactDir, "INITAPP.ELF")
	if err := prepareSeed0BootELF(workspace, seedELF, opts.BuildInit); err != nil {
		return Seed0BootResult{SeedELF: seedELF, Marker: marker, Timeout: opts.Timeout}, err
	}

	kernelEFI := workspace.Path(workspace.Kernel.Dir, "zig-out", "bin", "EFI", "BOOT", "BOOTX64.EFI")
	if stat, err := os.Stat(kernelEFI); err != nil {
		return Seed0BootResult{SeedELF: seedELF, KernelEFI: kernelEFI, Marker: marker, Timeout: opts.Timeout}, err
	} else if stat.Size() == 0 {
		return Seed0BootResult{SeedELF: seedELF, KernelEFI: kernelEFI, Marker: marker, Timeout: opts.Timeout}, fmt.Errorf("%s is empty", kernelEFI)
	}

	diskPath := workspace.Path(workspace.Disk.Image)
	mtoolsImage, err := espMToolsImage(workspace, diskPath)
	if err != nil {
		return Seed0BootResult{SeedELF: seedELF, KernelEFI: kernelEFI, Disk: diskPath, Marker: marker, Timeout: opts.Timeout}, err
	}

	initBackup := filepath.Join(artifactDir, "INITAPP.original.ELF")
	kernelBackup := filepath.Join(artifactDir, "BOOTX64.original.EFI")
	result := Seed0BootResult{
		SeedELF:   seedELF,
		KernelEFI: kernelEFI,
		Disk:      diskPath,
		Marker:    marker,
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
	if err := mcopyIn(mtoolsImage, seedELF, "::/EFI/BOOT/INITAPP.ELF"); err != nil {
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
		Marker:    marker,
	})
	result.Serial = smoke.Serial
	result.Log = smoke.Log

	restoreErr := restore()
	result.Restored = restored
	return result, errors.Join(smokeErr, restoreErr)
}

func prepareSeed0BootELF(workspace *config.Workspace, out string, build bool) error {
	built := workspace.Path(workspace.Artifacts, "cmake", "seed0boot", "seed0boot.elf")
	if build {
		cmd := exec.Command("bash", workspace.Path("tools", "build_seed0boot.sh"))
		cmd.Dir = workspace.Root
		if output, err := cmd.CombinedOutput(); err != nil {
			return fmt.Errorf("build seed0boot failed: %w\n%s", err, string(output))
		}
	}
	if stat, err := os.Stat(built); err != nil {
		return fmt.Errorf("seed0boot ELF is missing: %s (run tools/build_seed0boot.sh or pass --build-init)", built)
	} else if stat.Size() == 0 {
		return fmt.Errorf("seed0boot ELF is empty: %s", built)
	}
	if err := copyFile(out, built); err != nil {
		return err
	}
	return nil
}
