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

const muslPachaOSSmokeMarker = "[pachaos-musl-smoke] OK"

type MuslPachaOSSmokeOptions struct {
	Timeout   time.Duration
	NoKVM     bool
	ExtraArgs []string
}

type MuslPachaOSSmokeResult struct {
	SmokeELF  string
	KernelEFI string
	Disk      string
	Serial    string
	Log       string
	Marker    string
	Timeout   time.Duration
	Restored  bool
}

func RunMuslPachaOSSmoke(workspace *config.Workspace, opts MuslPachaOSSmokeOptions) (MuslPachaOSSmokeResult, error) {
	if opts.Timeout <= 0 {
		opts.Timeout = 30 * time.Second
	}
	artifactDir := workspace.Path(workspace.Artifacts, "boot-tests", "musl-pachaos-smoke")
	if err := os.MkdirAll(artifactDir, 0o755); err != nil {
		return MuslPachaOSSmokeResult{}, err
	}

	smokeELF := filepath.Join(artifactDir, "INITAPP.ELF")
	if err := buildMuslPachaOSSmokeELF(workspace, smokeELF); err != nil {
		return MuslPachaOSSmokeResult{SmokeELF: smokeELF, Marker: muslPachaOSSmokeMarker, Timeout: opts.Timeout}, err
	}

	kernelEFI := workspace.Path(workspace.Kernel.Dir, "zig-out", "bin", "EFI", "BOOT", "BOOTX64.EFI")
	if stat, err := os.Stat(kernelEFI); err != nil {
		return MuslPachaOSSmokeResult{SmokeELF: smokeELF, KernelEFI: kernelEFI, Marker: muslPachaOSSmokeMarker, Timeout: opts.Timeout}, err
	} else if stat.Size() == 0 {
		return MuslPachaOSSmokeResult{SmokeELF: smokeELF, KernelEFI: kernelEFI, Marker: muslPachaOSSmokeMarker, Timeout: opts.Timeout}, fmt.Errorf("%s is empty", kernelEFI)
	}

	diskPath := workspace.Path(workspace.Disk.Image)
	mtoolsImage, err := espMToolsImage(workspace, diskPath)
	if err != nil {
		return MuslPachaOSSmokeResult{SmokeELF: smokeELF, KernelEFI: kernelEFI, Disk: diskPath, Marker: muslPachaOSSmokeMarker, Timeout: opts.Timeout}, err
	}

	initBackup := filepath.Join(artifactDir, "INITAPP.original.ELF")
	kernelBackup := filepath.Join(artifactDir, "BOOTX64.original.EFI")
	result := MuslPachaOSSmokeResult{
		SmokeELF:  smokeELF,
		KernelEFI: kernelEFI,
		Disk:      diskPath,
		Marker:    muslPachaOSSmokeMarker,
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
		Marker:    muslPachaOSSmokeMarker,
	})
	result.Serial = smoke.Serial
	result.Log = smoke.Log

	restoreErr := restore()
	result.Restored = restored
	return result, errors.Join(smokeErr, restoreErr)
}

func buildMuslPachaOSSmokeELF(workspace *config.Workspace, out string) error {
	cmd := exec.Command("bash", workspace.Path("musl", "pachaos", "build", "build-smokes.sh"))
	cmd.Dir = workspace.Root
	if output, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("build-smokes.sh failed: %w\n%s", err, string(output))
	}
	built := workspace.Path(workspace.Artifacts, "musl-pachaos", "hello-libc-scaffold.elf")
	if err := copyFile(out, built); err != nil {
		return err
	}
	return nil
}

func copyFile(dst string, src string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, data, 0o644)
}
