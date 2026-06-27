package limine

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/progress"
)

const (
	imageName        = "limine-boot.img"
	imageSizeBytes   = int64(64 * 1024 * 1024)
	fatPartitionByte = int64(2 * 1024 * 1024)
)

type Inputs struct {
	KernelELF   string
	InitELF     string
	BootfsImage string
}

type Options struct {
	Limine   string
	Force    bool
	Progress progress.Reporter
}

type Result struct {
	Image      string
	Limine     string
	Config     string
	FilesBytes int64
}

func BuildImageWithOptions(workspace *config.Workspace, inputs Inputs, opts Options) (Result, error) {
	span := progress.Use(opts.Progress).Start("limine boot image", 8)
	defer span.Close()
	span.Set(1, "locating limine")
	limine, dataDir, err := locateLimine(opts.Limine)
	if err != nil {
		span.Fail("limine lookup failed")
		return Result{}, err
	}

	span.Set(2, "checking inputs")
	total, err := checkInputs(inputs, dataDir)
	if err != nil {
		span.Fail("limine input check failed")
		return Result{}, err
	}

	imagePath := workspace.Path(workspace.Artifacts, imageName)
	configPath := workspace.Path(workspace.Artifacts, "limine.conf")
	span.Set(3, "creating disk image")
	if err := os.MkdirAll(filepath.Dir(imagePath), 0o755); err != nil {
		span.Fail("limine image directory failed")
		return Result{}, err
	}
	if err := createEmptyImage(imagePath); err != nil {
		span.Fail("limine image create failed")
		return Result{}, err
	}

	span.Set(4, "partitioning disk image")
	if err := run("sgdisk", "--clear", "--new=1:2048:4095", "--typecode=1:ef02", "--change-name=1:limine-bios", "--new=2:4096:0", "--typecode=2:ef00", "--change-name=2:limine-esp", imagePath); err != nil {
		span.Fail("limine GPT failed")
		return Result{}, err
	}

	span.Set(5, "formatting FAT partition")
	mtoolsImage := fmt.Sprintf("%s@@%d", imagePath, fatPartitionByte)
	if err := run("mformat", "-i", mtoolsImage, "-F", "-v", "LIMINEBOOT", "::"); err != nil {
		span.Fail("limine FAT format failed")
		return Result{}, err
	}
	if err := run("mmd", "-i", mtoolsImage, "::/EFI"); err != nil {
		span.Fail("limine EFI dir failed")
		return Result{}, err
	}
	if err := run("mmd", "-i", mtoolsImage, "::/EFI/BOOT"); err != nil {
		span.Fail("limine EFI boot dir failed")
		return Result{}, err
	}

	span.Set(6, "copying boot files")
	if err := writeConfig(configPath); err != nil {
		span.Fail("limine config write failed")
		return Result{}, err
	}
	copies := [][2]string{
		{filepath.Join(dataDir, "limine-bios.sys"), "::/limine-bios.sys"},
		{filepath.Join(dataDir, "BOOTX64.EFI"), "::/EFI/BOOT/BOOTX64.EFI"},
		{inputs.KernelELF, "::/KERNEL.ELF"},
		{inputs.InitELF, "::/INITAPP.ELF"},
		{inputs.BootfsImage, "::/BOOTFS.IMG"},
		{configPath, "::/limine.conf"},
	}
	for _, item := range copies {
		if err := run("mcopy", "-o", "-i", mtoolsImage, item[0], item[1]); err != nil {
			span.Fail("limine file copy failed")
			return Result{}, err
		}
	}

	span.Set(7, "installing limine BIOS boot code")
	if err := run(limine, "bios-install", imagePath, "1"); err != nil {
		span.Fail("limine BIOS install failed")
		return Result{}, err
	}

	span.Set(8, "checking image")
	if info, err := os.Stat(imagePath); err != nil || info.Size() == 0 {
		span.Fail("limine image check failed")
		if err != nil {
			return Result{}, err
		}
		return Result{}, fmt.Errorf("empty limine image: %s", imagePath)
	}
	span.Done("limine boot image ready")
	return Result{Image: imagePath, Limine: limine, Config: configPath, FilesBytes: total}, nil
}

func locateLimine(explicit string) (string, string, error) {
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	if env := os.Getenv("CAPOS_LIMINE"); env != "" {
		candidates = append(candidates, env)
	}
	if found, err := exec.LookPath("limine"); err == nil {
		candidates = append(candidates, found)
	}
	if len(candidates) == 0 {
		out, err := exec.Command("nix", "build", "--no-link", "--print-out-paths", "nixpkgs#limine").Output()
		if err == nil {
			for _, line := range strings.Split(strings.TrimSpace(string(out)), "\n") {
				storePath := strings.TrimSpace(line)
				if storePath != "" {
					candidates = append(candidates, filepath.Join(storePath, "bin", "limine"))
				}
			}
		}
	}
	for _, candidate := range candidates {
		if candidate == "" {
			continue
		}
		data, err := exec.Command(candidate, "--print-datadir").Output()
		if err != nil {
			continue
		}
		dataDir := strings.TrimSpace(string(data))
		if _, err := os.Stat(filepath.Join(dataDir, "limine-bios.sys")); err != nil {
			continue
		}
		if _, err := os.Stat(filepath.Join(dataDir, "BOOTX64.EFI")); err != nil {
			continue
		}
		return candidate, dataDir, nil
	}
	return "", "", fmt.Errorf("limine not found; set CAPOS_LIMINE or install nixpkgs#limine")
}

func checkInputs(inputs Inputs, dataDir string) (int64, error) {
	var total int64
	for _, path := range []string{
		inputs.KernelELF,
		inputs.InitELF,
		inputs.BootfsImage,
		filepath.Join(dataDir, "limine-bios.sys"),
		filepath.Join(dataDir, "BOOTX64.EFI"),
	} {
		info, err := os.Stat(path)
		if err != nil {
			return 0, err
		}
		if info.Size() == 0 {
			return 0, fmt.Errorf("empty limine input: %s", path)
		}
		total += info.Size()
	}
	return total, nil
}

func createEmptyImage(path string) error {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer file.Close()
	return file.Truncate(imageSizeBytes)
}

func writeConfig(path string) error {
	content := "" +
		"timeout: 0\n" +
		"serial: yes\n" +
		"graphics: no\n" +
		"verbose: yes\n" +
		"\n" +
		"/pacha\n" +
		"    protocol: limine\n" +
		"    path: boot():/KERNEL.ELF\n" +
		"    resolution: 640x480x32\n"
	if existing, err := os.ReadFile(path); err == nil && bytes.Equal(existing, []byte(content)) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(content), 0o644)
}

func run(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	var combined bytes.Buffer
	cmd.Stdout = &combined
	cmd.Stderr = &combined
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s %s failed: %w\n%s", name, strings.Join(args, " "), err, combined.String())
	}
	return nil
}
