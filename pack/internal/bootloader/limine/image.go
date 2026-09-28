package limine

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"capabilityos/pack/internal/config"
	"capabilityos/pack/internal/imagelock"
	"capabilityos/pack/internal/progress"
)

const (
	imageName          = "limine-boot.img"
	minImageSizeBytes  = int64(64 * 1024 * 1024)
	imageSizeStepBytes = int64(64 * 1024 * 1024)
	fatPartitionByte   = int64(2 * 1024 * 1024)
)

type Inputs struct {
	KernelELF     string
	InitELF       string
	BootfsImage   string
	DiagnosticELF string // Optional manual rescue entry for real-hardware live media.
}

type Options struct {
	Limine     string
	Force      bool
	ImagePath  string
	ConfigPath string
	Progress   progress.Reporter
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
	if opts.ImagePath != "" {
		imagePath = opts.ImagePath
	}
	if opts.ConfigPath != "" {
		configPath = opts.ConfigPath
	}
	locks, err := imagelock.Acquire(imagePath)
	if err != nil {
		span.Fail("limine image lock failed")
		return Result{}, err
	}
	defer locks.Close()
	span.Set(3, "creating disk image")
	if err := os.MkdirAll(filepath.Dir(imagePath), 0o755); err != nil {
		span.Fail("limine image directory failed")
		return Result{}, err
	}
	// USB file updates use these same snapshots. A stale loose KERNEL.ELF
	// must never accompany a newly built, QEMU-tested image.
	inputs, err = stageBootInputs(filepath.Dir(imagePath), inputs)
	if err != nil {
		span.Fail("limine input staging failed")
		return Result{}, err
	}
	if err := createEmptyImage(imagePath, total); err != nil {
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
	imageInfo, err := os.Stat(imagePath)
	if err != nil {
		return Result{}, err
	}
	sectors, err := fatPartitionSectors(imageInfo.Size())
	if err != nil {
		return Result{}, err
	}
	if err := run("mformat", "-i", mtoolsImage, "-T", fmt.Sprint(sectors), "-F", "-v", "LIMINEBOOT", "::"); err != nil {
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
	if err := writeConfig(configPath, inputs.DiagnosticELF != ""); err != nil {
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
	if inputs.DiagnosticELF != "" {
		copies = append(copies, [2]string{inputs.DiagnosticELF, "::/DIAGBOOT.ELF"})
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

func fatPartitionSectors(imageBytes int64) (int64, error) {
	// An offset alone lets mtools consume the backup GPT. Keep the FAT BPB
	// extent equal to partition 2, excluding its 32 table sectors and header.
	const backupGPTSectors = int64(33)
	if imageBytes%512 != 0 || imageBytes <= fatPartitionByte+backupGPTSectors*512 {
		return 0, fmt.Errorf("invalid Limine image extent: %d", imageBytes)
	}
	return imageBytes/512 - fatPartitionByte/512 - backupGPTSectors, nil
}

func stageBootInputs(dir string, inputs Inputs) (Inputs, error) {
	for _, item := range []struct {
		path *string
		name string
	}{
		{&inputs.KernelELF, "KERNEL.ELF"},
		{&inputs.InitELF, "INITAPP.ELF"},
		{&inputs.BootfsImage, "BOOTFS.IMG"},
		{&inputs.DiagnosticELF, "DIAGBOOT.ELF"},
	} {
		if *item.path == "" {
			continue
		}
		destination := filepath.Join(dir, item.name)
		if err := copyBootInput(*item.path, destination); err != nil {
			return Inputs{}, fmt.Errorf("stage %s: %w", item.name, err)
		}
		*item.path = destination
	}
	return inputs, nil
}

func copyBootInput(source, destination string) error {
	in, err := os.Open(source)
	if err != nil {
		return err
	}
	defer in.Close()
	sourceInfo, err := in.Stat()
	if err != nil {
		return err
	}
	if destinationInfo, err := os.Stat(destination); err == nil && os.SameFile(sourceInfo, destinationInfo) {
		return nil
	}
	out, err := os.CreateTemp(filepath.Dir(destination), ".boot-input-*")
	if err != nil {
		return err
	}
	defer os.Remove(out.Name())
	defer out.Close()
	if err := out.Chmod(0o644); err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	return os.Rename(out.Name(), destination)
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
	paths := []string{
		inputs.KernelELF,
		inputs.InitELF,
		inputs.BootfsImage,
		filepath.Join(dataDir, "limine-bios.sys"),
		filepath.Join(dataDir, "BOOTX64.EFI"),
	}
	if inputs.DiagnosticELF != "" {
		paths = append(paths, inputs.DiagnosticELF)
	}
	for _, path := range paths {
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

func createEmptyImage(path string, filesBytes int64) error {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer file.Close()
	// FAT metadata and boot partitions need room beyond input files. Ordinary
	// images remain 64 MiB; optional Mesa images grow in bounded steps.
	bytes := filesBytes + 32*1024*1024
	if bytes < minImageSizeBytes {
		bytes = minImageSizeBytes
	}
	bytes = (bytes + imageSizeStepBytes - 1) / imageSizeStepBytes * imageSizeStepBytes
	return file.Truncate(bytes)
}

func writeConfig(path string, diagnostic bool) error {
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
	if diagnostic {
		content = "" +
			"timeout: 3\n" +
			"default_entry: 1\n" +
			"serial: yes\n" +
			"graphics: no\n" +
			"verbose: yes\n" +
			"\n" +
			"/PachaOS live (RAM only)\n" +
			"    protocol: limine\n" +
			"    path: boot():/KERNEL.ELF\n" +
			"    resolution: 640x480x32\n" +
			"\n" +
			"/Hardware diagnostic (manual)\n" +
			"    protocol: limine\n" +
			"    path: boot():/DIAGBOOT.ELF\n" +
			"    resolution: 640x480x32\n"
	}
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
