package limine

import (
	"bytes"
	"crypto/sha256"
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
	// InstallerPayload is an explicitly selected rootfs archive. It is never
	// inferred from the ordinary rootfs or copied into the RAM bootfs.
	InstallerPayload string
	// InstallerSystemBootfs is the ordinary storage-boot archive installed as
	// BOOTFS.IMG. It must not be the live/installer RAM archive.
	InstallerSystemBootfs string
	// An optional desktop package is kept on the install-data partition. It is
	// never part of the installed OS payload or the boot-time RAM archive.
	LiveDesktopBundle string
	// InstallerUEFIEntry is the removable-media entry which registers an
	// installed fixed disk before chainloading its unmodified Limine image.
	// It is required only for installation media; ordinary live images keep
	// Limine directly at the standard fallback path.
	InstallerUEFIEntry string
	Progress           progress.Reporter
}

type Result struct {
	Image      string
	Limine     string
	Config     string
	FilesBytes int64
	Skipped    bool
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
	if opts.InstallerPayload != "" {
		if opts.InstallerSystemBootfs == "" {
			return Result{}, fmt.Errorf("installer system bootfs is required with installer payload")
		}
		if opts.InstallerUEFIEntry == "" {
			return Result{}, fmt.Errorf("installer UEFI entry is required with installer payload")
		}
		payloadPath, err := filepath.Abs(opts.InstallerPayload)
		if err != nil {
			return Result{}, err
		}
		outputPath, err := filepath.Abs(imagePath)
		if err != nil {
			return Result{}, err
		}
		if payloadPath == outputPath {
			return Result{}, fmt.Errorf("installer payload cannot be the output image")
		}
	}
	if opts.LiveDesktopBundle != "" && opts.InstallerPayload == "" {
		return Result{}, fmt.Errorf("desktop bundle requires installer data partition")
	}
	locks, err := imagelock.Acquire(imagePath)
	if err != nil {
		span.Fail("limine image lock failed")
		return Result{}, err
	}
	defer locks.Close()
	// Installer images embed payloads that are not part of the record; they
	// are always rebuilt.
	var digests map[string]string
	if opts.InstallerPayload == "" {
		digests, err = imageInputDigests(workspace, inputs, limine, dataDir)
		if err != nil {
			span.Fail("limine input digest failed")
			return Result{}, err
		}
		if !opts.Force {
			fresh, err := imageMatchesRecord(workspace, imagePath, digests)
			if err != nil {
				span.Fail("limine image check failed")
				return Result{}, err
			}
			if fresh {
				if _, err := stageBootInputs(filepath.Dir(imagePath), inputs); err != nil {
					span.Fail("limine input staging failed")
					return Result{}, err
				}
				if err := writeConfig(configPath, inputs.DiagnosticELF != ""); err != nil {
					span.Fail("limine config write failed")
					return Result{}, err
				}
				span.Done("limine boot image up-to-date")
				return Result{Image: imagePath, Limine: limine, Config: configPath, FilesBytes: total, Skipped: true}, nil
			}
		}
	}
	if err := removeImageRecord(imagePath); err != nil {
		span.Fail("limine image record invalidation failed")
		return Result{}, err
	}
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
	bootImageBytes, err := imageSize(imagePath)
	if err != nil {
		return Result{}, err
	}
	installerBytes := int64(0)
	if opts.InstallerPayload != "" {
		payload, statErr := os.Stat(opts.InstallerPayload)
		if statErr != nil {
			return Result{}, statErr
		}
		if !payload.Mode().IsRegular() || payload.Size() == 0 || payload.Size() >= 4<<30 {
			return Result{}, fmt.Errorf("installer payload must be a nonempty FAT-compatible regular file")
		}
		system, statErr := os.Stat(opts.InstallerSystemBootfs)
		if statErr != nil {
			return Result{}, statErr
		}
		if !system.Mode().IsRegular() || system.Size() == 0 {
			return Result{}, fmt.Errorf("installer system bootfs must be a nonempty regular file")
		}
		desktopBytes := int64(0)
		if opts.LiveDesktopBundle != "" {
			desktop, statErr := os.Stat(opts.LiveDesktopBundle)
			if statErr != nil {
				return Result{}, statErr
			}
			if !desktop.Mode().IsRegular() || desktop.Size() == 0 || desktop.Size() >= 4<<30 {
				return Result{}, fmt.Errorf("desktop bundle must be a nonempty FAT-compatible regular file")
			}
			desktopBytes = desktop.Size()
		}
		installerBytes = roundImageBytes(payload.Size() + system.Size() + desktopBytes + 64<<20)
		if err := os.Truncate(imagePath, bootImageBytes+installerBytes); err != nil {
			return Result{}, err
		}
	}

	span.Set(4, "partitioning disk image")
	if err := partitionImage(imagePath, bootImageBytes, installerBytes); err != nil {
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
	if installerBytes != 0 {
		sectors = bootImageBytes/512 - fatPartitionByte/512
	}
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
	if opts.InstallerPayload != "" {
		if err := run("mmd", "-i", mtoolsImage, "::/EFI/PachaOS"); err != nil {
			span.Fail("installer EFI vendor dir failed")
			return Result{}, err
		}
	}

	span.Set(6, "copying boot files")
	if err := writeConfig(configPath, inputs.DiagnosticELF != ""); err != nil {
		span.Fail("limine config write failed")
		return Result{}, err
	}
	bootEntry := filepath.Join(dataDir, "BOOTX64.EFI")
	copies := [][2]string{
		{filepath.Join(dataDir, "limine-bios.sys"), "::/limine-bios.sys"},
		{bootEntry, "::/EFI/BOOT/BOOTX64.EFI"},
		{inputs.KernelELF, "::/KERNEL.ELF"},
		{inputs.InitELF, "::/INITAPP.ELF"},
		{inputs.BootfsImage, "::/BOOTFS.IMG"},
		{configPath, "::/limine.conf"},
	}
	if opts.InstallerPayload != "" {
		// The installer entry owns only pre-ExitBootServices registration and
		// chainloading.  Limine remains byte-for-byte upstream and is also the
		// loader copied to the target fixed disk.
		copies[1][0] = opts.InstallerUEFIEntry
		copies = append(copies,
			[2]string{bootEntry, "::/EFI/PachaOS/LIMINE.EFI"})
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
	if installerBytes != 0 {
		installedConfig := filepath.Join(filepath.Dir(imagePath), "installed-limine.conf")
		if err := writeConfig(installedConfig, false); err != nil {
			return Result{}, fmt.Errorf("installed limine config: %w", err)
		}
		if err := writeInstallerPayload(imagePath, bootImageBytes,
			installerBytes, opts.InstallerPayload,
			opts.InstallerSystemBootfs, installedConfig,
			inputs.KernelELF, inputs.InitELF, bootEntry); err != nil {
			return Result{}, err
		}
		if opts.LiveDesktopBundle != "" {
			if err := writeDesktopPayload(imagePath, bootImageBytes,
				opts.LiveDesktopBundle); err != nil {
				return Result{}, err
			}
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
	if digests != nil {
		if err := writeImageRecord(workspace, imagePath, digests); err != nil {
			span.Fail("limine image record failed")
			return Result{}, err
		}
	}
	span.Done("limine boot image ready")
	return Result{Image: imagePath, Limine: limine, Config: configPath, FilesBytes: total}, nil
}

func partitionImage(imagePath string, bootImageBytes, installerBytes int64) error {
	partitionArgs := []string{"--clear", "--new=1:2048:4095", "--typecode=1:ef02", "--change-name=1:limine-bios"}
	if installerBytes == 0 {
		partitionArgs = append(partitionArgs, "--new=2:4096:0", "--typecode=2:ef00", "--change-name=2:limine-esp")
	} else {
		partitionArgs = append(partitionArgs,
			fmt.Sprintf("--new=2:4096:%d", bootImageBytes/512-1),
			"--typecode=2:ef00", "--change-name=2:pacha-live-esp",
			fmt.Sprintf("--new=3:%d:%d", bootImageBytes/512,
				(bootImageBytes+installerBytes)/512-2049),
			"--typecode=3:0700", "--change-name=3:pacha-install-data")
	}
	partitionArgs = append(partitionArgs, imagePath)
	return run("sgdisk", partitionArgs...)
}

func writeInstallerPayload(imagePath string, bootImageBytes, installerBytes int64,
	payload, systemBootfs, systemConfig, kernel, init, limineEFI string) error {
	dataImage := fmt.Sprintf("%s@@%d", imagePath, bootImageBytes)
	dataSectors := installerBytes/512 - 2048
	if err := run("mformat", "-i", dataImage, "-T", fmt.Sprint(dataSectors), "-F", "-v", "PACHA-DATA", "::"); err != nil {
		return fmt.Errorf("installer data format: %w", err)
	}
	if err := run("mcopy", "-o", "-i", dataImage, payload, "::/ROOTFS.TAR"); err != nil {
		return fmt.Errorf("installer rootfs copy: %w", err)
	}
	if err := verifyFATFile(imagePath, bootImageBytes, "ROOTFS.TAR", payload); err != nil {
		return fmt.Errorf("installer rootfs readback: %w", err)
	}
	opened, err := os.Open(payload)
	if err != nil {
		return err
	}
	h := sha256.New()
	_, err = io.Copy(h, opened)
	closeErr := opened.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	manifest, err := os.CreateTemp(filepath.Dir(imagePath), ".rootfs-digest-*")
	if err != nil {
		return err
	}
	defer os.Remove(manifest.Name())
	if _, err := fmt.Fprintf(manifest, "%x  ROOTFS.TAR\n", h.Sum(nil)); err != nil {
		manifest.Close()
		return err
	}
	if err := manifest.Close(); err != nil {
		return err
	}
	if err := run("mcopy", "-o", "-i", dataImage, manifest.Name(), "::/ROOTFS.SHA"); err != nil {
		return err
	}
	if err := verifyFATFile(imagePath, bootImageBytes, "ROOTFS.SHA", manifest.Name()); err != nil {
		return err
	}
	if err := run("mcopy", "-o", "-i", dataImage, systemBootfs, "::/SYSTEM.BOOTFS"); err != nil {
		return fmt.Errorf("installer system bootfs copy: %w", err)
	}
	if err := verifyFATFile(imagePath, bootImageBytes, "SYSTEM.BOOTFS", systemBootfs); err != nil {
		return fmt.Errorf("installer system bootfs readback: %w", err)
	}
	if err := run("mcopy", "-o", "-i", dataImage, systemConfig, "::/SYSTEM.CONF"); err != nil {
		return fmt.Errorf("installer system config copy: %w", err)
	}
	if err := verifyFATFile(imagePath, bootImageBytes, "SYSTEM.CONF", systemConfig); err != nil {
		return fmt.Errorf("installer system config readback: %w", err)
	}
	systemFile, err := os.Open(systemBootfs)
	if err != nil {
		return err
	}
	systemHash := sha256.New()
	_, err = io.Copy(systemHash, systemFile)
	closeErr = systemFile.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	systemManifest, err := os.CreateTemp(filepath.Dir(imagePath), ".system-digest-*")
	if err != nil {
		return err
	}
	defer os.Remove(systemManifest.Name())
	if _, err := fmt.Fprintf(systemManifest, "%x  SYSTEM.BOOTFS\n", systemHash.Sum(nil)); err != nil {
		systemManifest.Close()
		return err
	}
	if err := systemManifest.Close(); err != nil {
		return err
	}
	if err := run("mcopy", "-o", "-i", dataImage, systemManifest.Name(), "::/SYSTEM.SHA"); err != nil {
		return err
	}
	if err := verifyFATFile(imagePath, bootImageBytes, "SYSTEM.SHA", systemManifest.Name()); err != nil {
		return err
	}
	paths := []string{payload, systemBootfs, kernel, init, limineEFI, systemConfig}
	sizes := make([]int64, len(paths))
	for i, path := range paths {
		info, err := os.Stat(path)
		if err != nil || !info.Mode().IsRegular() || info.Size() <= 0 {
			return fmt.Errorf("installer metadata input %q is not a nonempty regular file", path)
		}
		sizes[i] = info.Size()
	}
	metadata, err := os.CreateTemp(filepath.Dir(imagePath), ".installer-meta-*")
	if err != nil {
		return err
	}
	defer os.Remove(metadata.Name())
	/* Numeric-only, fixed-field metadata is parsed by ash without evaluating
	 * media-controlled text.  These are payload byte counts, not guessed
	 * percentages; the runtime validates every field before reporting them. */
	if _, err := fmt.Fprintf(metadata, "1 %d %d %d %d %d %d\n",
		sizes[0], sizes[1], sizes[2], sizes[3], sizes[4], sizes[5]); err != nil {
		metadata.Close()
		return err
	}
	if err := metadata.Close(); err != nil {
		return err
	}
	if err := run("mcopy", "-o", "-i", dataImage,
		metadata.Name(), "::/INSTALL.META"); err != nil {
		return fmt.Errorf("installer size metadata copy: %w", err)
	}
	if err := verifyFATFile(imagePath, bootImageBytes,
		"INSTALL.META", metadata.Name()); err != nil {
		return fmt.Errorf("installer size metadata readback: %w", err)
	}
	return nil
}

func writeDesktopPayload(imagePath string, bootImageBytes int64, bundle string) error {
	dataImage := fmt.Sprintf("%s@@%d", imagePath, bootImageBytes)
	if err := run("mcopy", "-o", "-i", dataImage, bundle, "::/DESKTOP.TGZ"); err != nil {
		return fmt.Errorf("desktop bundle copy: %w", err)
	}
	if err := verifyFATFile(imagePath, bootImageBytes, "DESKTOP.TGZ", bundle); err != nil {
		return fmt.Errorf("desktop bundle readback: %w", err)
	}
	file, err := os.Open(bundle)
	if err != nil {
		return err
	}
	h := sha256.New()
	_, err = io.Copy(h, file)
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	digest, err := os.CreateTemp(filepath.Dir(imagePath), ".desktop-digest-*")
	if err != nil {
		return err
	}
	defer os.Remove(digest.Name())
	if _, err := fmt.Fprintf(digest, "%x  DESKTOP.TGZ\n", h.Sum(nil)); err != nil {
		digest.Close()
		return err
	}
	if err := digest.Close(); err != nil {
		return err
	}
	if err := run("mcopy", "-o", "-i", dataImage, digest.Name(), "::/DESKTOP.SHA"); err != nil {
		return fmt.Errorf("desktop digest copy: %w", err)
	}
	if err := verifyFATFile(imagePath, bootImageBytes, "DESKTOP.SHA", digest.Name()); err != nil {
		return fmt.Errorf("desktop digest readback: %w", err)
	}
	return nil
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
	if destinationInfo, err := os.Stat(destination); err == nil {
		if os.SameFile(sourceInfo, destinationInfo) {
			return nil
		}
		same, err := sameContents(source, destination, sourceInfo.Size(), destinationInfo.Size())
		if err != nil {
			return err
		}
		if same {
			return nil
		}
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
	return file.Truncate(roundImageBytes(filesBytes + 32*1024*1024))
}

func roundImageBytes(bytes int64) int64 {
	if bytes < minImageSizeBytes {
		bytes = minImageSizeBytes
	}
	return (bytes + imageSizeStepBytes - 1) / imageSizeStepBytes * imageSizeStepBytes
}

func imageSize(path string) (int64, error) {
	info, err := os.Stat(path)
	if err != nil {
		return 0, err
	}
	return info.Size(), nil
}

func verifyFATFile(image string, offset int64, name, source string) error {
	readback, err := os.CreateTemp(filepath.Dir(image), ".installer-readback-*")
	if err != nil {
		return err
	}
	path := readback.Name()
	if err := readback.Close(); err != nil {
		os.Remove(path)
		return err
	}
	defer os.Remove(path)
	if err := run("mcopy", "-o", "-i", fmt.Sprintf("%s@@%d", image, offset), "::/"+name, path); err != nil {
		return err
	}
	var sums [2][32]byte
	for i, file := range []string{source, path} {
		opened, err := os.Open(file)
		if err != nil {
			return err
		}
		h := sha256.New()
		_, err = io.Copy(h, opened)
		closeErr := opened.Close()
		if err != nil {
			return err
		}
		if closeErr != nil {
			return closeErr
		}
		copy(sums[i][:], h.Sum(nil))
	}
	if sums[0] != sums[1] {
		return fmt.Errorf("FAT readback digest mismatch")
	}
	return nil
}

func writeConfig(path string, diagnostic bool) error {
	content := configContent(diagnostic)
	if existing, err := os.ReadFile(path); err == nil && bytes.Equal(existing, []byte(content)) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(content), 0o644)
}

func configContent(diagnostic bool) string {
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
	return content
}

func sha256Bytes(data []byte) []byte {
	sum := sha256.Sum256(data)
	return sum[:]
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

func sameContents(lhs, rhs string, lhsSize, rhsSize int64) (bool, error) {
	if lhsSize != rhsSize {
		return false, nil
	}
	var sums [2][]byte
	for index, path := range []string{lhs, rhs} {
		file, err := os.Open(path)
		if err != nil {
			return false, err
		}
		hash := sha256.New()
		_, err = io.Copy(hash, file)
		closeErr := file.Close()
		if err != nil {
			return false, err
		}
		if closeErr != nil {
			return false, closeErr
		}
		sums[index] = hash.Sum(nil)
	}
	return bytes.Equal(sums[0], sums[1]), nil
}
