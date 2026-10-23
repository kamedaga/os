package livebootfs

import (
	"bytes"
	"crypto/sha256"
	"debug/elf"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"

	"capabilityos/pack/internal/bootfs"
	"capabilityos/pack/internal/buildsys"
	"capabilityos/pack/internal/config"
	"gopkg.in/yaml.v3"
)

const (
	SpecPath     = "pack/live-bootfs.yaml"
	OutputDir    = ".artifacts/live-bootfs"
	ManifestName = "bootfs.generated.txt"
	ImageName    = "BOOTFS.IMG"
	// Conservative packaging budget for seed0boot and staging metadata. The
	// kernel scratch arena grows on demand; this is not a fixed allocation.
	// Bootfs contents are staged in physical pages outside this budget.
	bootScratchBytes = 32 * 1024 * 1024
	maxBootfsBytes   = 192 * 1024 * 1024
	maxLiveRootBytes = 192 * 1024 * 1024
)

type Spec struct {
	Schema int               `yaml:"schema"`
	SHA256 map[string]string `yaml:"sha256"`
	Files  map[string]string `yaml:"files"`
}

type Options struct {
	NoBuild            bool
	AuthorizedKeysPath string
	OverlaySpecPath    string
	InstallerMode      bool
	OutputDir          string
}

type Result struct {
	Manifest string
	Image    string
	Entries  int
	Bytes    int64
}

func LoadSpec(filename string) (Spec, error) {
	data, err := os.ReadFile(filename)
	if err != nil {
		return Spec{}, err
	}
	var spec Spec
	decoder := yaml.NewDecoder(bytes.NewReader(data))
	decoder.KnownFields(true)
	if err := decoder.Decode(&spec); err != nil {
		return Spec{}, fmt.Errorf("live bootfs spec: %w", err)
	}
	if spec.Schema != 1 || len(spec.Files) == 0 {
		return Spec{}, fmt.Errorf("live bootfs spec: unsupported schema or empty files")
	}
	for destination, appID := range spec.Files {
		if destination == "/" || !strings.HasPrefix(destination, "/") ||
			path.Clean(destination) != destination || strings.ContainsAny(destination, "=\n\r") ||
			appID == "" {
			return Spec{}, fmt.Errorf("live bootfs spec: invalid entry %q: %q", destination, appID)
		}
	}
	for appID, digest := range spec.SHA256 {
		if len(digest) != 64 {
			return Spec{}, fmt.Errorf("live bootfs spec: invalid sha256 for %s", appID)
		}
		if _, err := hex.DecodeString(digest); err != nil {
			return Spec{}, fmt.Errorf("live bootfs spec: invalid sha256 for %s: %w", appID, err)
		}
	}
	return spec, nil
}

func Build(workspace *config.Workspace, opts Options) (Result, error) {
	if workspace == nil {
		return Result{}, fmt.Errorf("live bootfs: nil workspace")
	}
	spec, err := LoadSpec(workspace.Path(SpecPath))
	if err != nil {
		return Result{}, err
	}
	overlays := []string{}
	if opts.OverlaySpecPath != "" {
		overlays = append(overlays, opts.OverlaySpecPath)
	}
	if opts.InstallerMode {
		overlays = append(overlays, "pack/installer-bootfs.yaml")
	}
	for _, overlayPath := range overlays {
		if opts.OutputDir == "" {
			return Result{}, fmt.Errorf("live bootfs: an overlay requires a separate output directory")
		}
		overlay, err := LoadSpec(workspace.Path(overlayPath))
		if err != nil {
			return Result{}, err
		}
		for destination, appID := range overlay.Files {
			if _, exists := spec.Files[destination]; exists {
				return Result{}, fmt.Errorf("live bootfs: overlay replaces %s", destination)
			}
			spec.Files[destination] = appID
		}
		for appID, digest := range overlay.SHA256 {
			if existing, exists := spec.SHA256[appID]; exists && existing != digest {
				return Result{}, fmt.Errorf("live bootfs: overlay changes sha256 for %s", appID)
			}
			spec.SHA256[appID] = digest
		}
	}
	selected := make(map[string]config.App)
	paths := make([]string, 0, len(spec.Files))
	for destination, appID := range spec.Files {
		app, ok := workspace.App(appID)
		if !ok || app.Kind() != "file" {
			return Result{}, fmt.Errorf("live bootfs: unknown file app %q for %s", appID, destination)
		}
		selected[appID] = app
		paths = append(paths, destination)
	}
	sort.Strings(paths)
	for appID := range spec.SHA256 {
		if _, ok := selected[appID]; !ok {
			return Result{}, fmt.Errorf("live bootfs: checksum for unselected app %s", appID)
		}
	}
	if !opts.NoBuild {
		// Build only this profile's apps. Never run the normal rootfs/ESP sync.
		profile := *workspace
		profile.AppsMap = make(map[string]config.App, len(selected)+1)
		for id, app := range selected {
			profile.AppsMap[id] = app
		}
		initApp, ok := workspace.App("seed0boot")
		if !ok {
			return Result{}, fmt.Errorf("live bootfs: missing seed0boot app")
		}
		profile.AppsMap["seed0boot"] = initApp
		profile.Skip = config.Skip{}
		if _, err := buildsys.BuildUserland(&profile, buildsys.UserlandOptions{}); err != nil {
			return Result{}, err
		}
	}
	initApp, ok := workspace.App("seed0boot")
	if !ok {
		return Result{}, fmt.Errorf("live bootfs: missing seed0boot app")
	}
	initBytes, initLoadBytes, err := bootInitScratchUse(workspace.ArtifactPath(initApp))
	if err != nil {
		return Result{}, err
	}
	if !fitsBootScratch(initBytes, initLoadBytes, 0) {
		return Result{}, fmt.Errorf("live bootfs: seed0boot alone exceeds %d-byte boot scratch", bootScratchBytes)
	}

	outputDir := workspace.Path(OutputDir)
	if opts.OutputDir != "" {
		outputDir = workspace.Path(opts.OutputDir)
	}
	manifestPath := filepath.Join(outputDir, ManifestName)
	imagePath := filepath.Join(outputDir, ImageName)
	var manifest strings.Builder
	manifest.WriteString("# Generated by pacgo live-bootfs. DO NOT EDIT.\n")
	var upperBound int64 = 104 + int64(len(paths))*56
	available := make(map[string]bool, len(paths))
	countedArtifact := make(map[string]bool, len(paths))
	checkedDigest := make(map[string]bool, len(spec.SHA256))
	checkedSource := make(map[string]bool, len(selected))
	for _, destination := range paths {
		app := selected[spec.Files[destination]]
		artifact := workspace.ArtifactPath(app)
		info, err := os.Lstat(artifact)
		if err != nil {
			return Result{}, fmt.Errorf("live bootfs: missing %s (%s): %w", destination, app.ID, err)
		}
		if !info.Mode().IsRegular() || info.Size() == 0 {
			return Result{}, fmt.Errorf("live bootfs: %s must be a nonempty regular file", artifact)
		}
		if opts.NoBuild && !checkedSource[app.ID] {
			if err := checkStagedSource(workspace, app, artifact); err != nil {
				return Result{}, err
			}
			checkedSource[app.ID] = true
		}
		if expected := spec.SHA256[app.ID]; expected != "" && !checkedDigest[app.ID] {
			data, err := os.ReadFile(artifact)
			if err != nil {
				return Result{}, err
			}
			actual := fmt.Sprintf("%x", sha256.Sum256(data))
			if actual != expected {
				return Result{}, fmt.Errorf("live bootfs: %s checksum mismatch: got %s, want %s", app.ID, actual, expected)
			}
			checkedDigest[app.ID] = true
		}
		upperBound += int64(len(destination)) + 8
		if !countedArtifact[artifact] {
			upperBound += info.Size()
			countedArtifact[artifact] = true
		}
		available[destination] = true
		rel, err := filepath.Rel(outputDir, artifact)
		if err != nil {
			return Result{}, err
		}
		fmt.Fprintf(&manifest, "%s=%s\n", destination, filepath.ToSlash(rel))
	}
	if opts.AuthorizedKeysPath != "" {
		keyPath, err := filepath.Abs(opts.AuthorizedKeysPath)
		if err != nil {
			return Result{}, err
		}
		key, err := os.ReadFile(keyPath)
		if err != nil {
			return Result{}, fmt.Errorf("live bootfs: SSH public key: %w", err)
		}
		if err := validateAuthorizedKey(key); err != nil {
			return Result{}, fmt.Errorf("live bootfs: SSH public key: %w", err)
		}
		const destination = "/root/.ssh/authorized_keys"
		rel, err := filepath.Rel(outputDir, keyPath)
		if err != nil {
			return Result{}, err
		}
		fmt.Fprintf(&manifest, "%s=%s\n", destination, filepath.ToSlash(rel))
		upperBound += int64(len(destination) + len(key) + 64)
	}
	if upperBound >= maxLiveRootBytes {
		return Result{}, fmt.Errorf("live bootfs: estimated RAM root %d bytes exceeds %d-byte limit", upperBound, maxLiveRootBytes)
	}
	if err := validateELFClosure(workspace, spec, paths, available); err != nil {
		return Result{}, err
	}
	if err := os.MkdirAll(outputDir, 0o755); err != nil {
		return Result{}, err
	}
	if err := writeIfChanged(manifestPath, []byte(manifest.String())); err != nil {
		return Result{}, err
	}
	rootImagePath := filepath.Join(outputDir, "ROOTFS.IMG")
	image, err := bootfs.BuildImage(manifestPath, rootImagePath)
	if err != nil {
		return Result{}, err
	}
	if image.Bytes >= maxLiveRootBytes {
		return Result{}, fmt.Errorf("live bootfs: RAM root archive %d bytes exceeds %d-byte limit", image.Bytes, maxLiveRootBytes)
	}
	if !fitsBootScratch(initBytes, initLoadBytes, int64(image.Bytes)) {
		return Result{}, fmt.Errorf("live bootfs: archive %d bytes exceeds boot staging capacity", image.Bytes)
	}
	// Directly stage the bounded archive in scattered boot pages. A packed
	// inner root needs one process-sized decompression allocation and fails
	// for Mesa's 155 MiB runtime even when physical RAM is abundant.
	data, err := os.ReadFile(rootImagePath)
	if err != nil {
		return Result{}, err
	}
	if err := writeIfChanged(imagePath, data); err != nil {
		return Result{}, err
	}
	return Result{Manifest: manifestPath, Image: imagePath, Entries: image.Entries, Bytes: int64(image.Bytes)}, nil
}

// --no-build may reuse binaries without their compiler/source tree, but a
// directly copied script still present in the workspace must match staging.
// Otherwise packaging succeeds with an old script despite a source fix.
func checkStagedSource(workspace *config.Workspace, app config.App, artifact string) error {
	source, err := app.FileSource()
	if err != nil {
		return err
	}
	if len(source.Rebuild) != 0 {
		return nil
	}
	current, err := os.ReadFile(workspace.Path(source.Path))
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	staged, err := os.ReadFile(artifact)
	if err != nil {
		return err
	}
	if !bytes.Equal(current, staged) {
		return fmt.Errorf("live bootfs: stale staged file for %s; run pacgo build userland --no-rootfs %s before --no-build", app.ID, app.ID)
	}
	return nil
}

func validateAuthorizedKey(data []byte) error {
	if len(data) == 0 || len(data) > 4096 {
		return fmt.Errorf("expected one Ed25519 public key (at most 4096 bytes)")
	}
	line := strings.TrimSuffix(string(data), "\n")
	if strings.ContainsAny(line, "\r\n") {
		return fmt.Errorf("expected exactly one public-key line")
	}
	fields := strings.Fields(line)
	if len(fields) < 2 || fields[0] != "ssh-ed25519" {
		return fmt.Errorf("expected an ssh-ed25519 public key without options")
	}
	decoded, err := base64.StdEncoding.DecodeString(fields[1])
	if err != nil || len(decoded) != 51 ||
		binary.BigEndian.Uint32(decoded[:4]) != 11 ||
		string(decoded[4:15]) != "ssh-ed25519" ||
		binary.BigEndian.Uint32(decoded[15:19]) != 32 {
		return fmt.Errorf("invalid Ed25519 public-key payload")
	}
	return nil
}

func bootInitScratchUse(filename string) (imageBytes, loadBytes int64, err error) {
	info, err := os.Stat(filename)
	if err != nil {
		return 0, 0, fmt.Errorf("live bootfs: seed0boot image: %w", err)
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 {
		return 0, 0, fmt.Errorf("live bootfs: seed0boot image must be a nonempty regular file")
	}
	image, err := elf.Open(filename)
	if err != nil {
		return 0, 0, fmt.Errorf("live bootfs: seed0boot ELF: %w", err)
	}
	defer image.Close()
	var maxEnd uint64
	for _, program := range image.Progs {
		if program.Type != elf.PT_LOAD || program.Memsz == 0 {
			continue
		}
		if program.Vaddr >= bootScratchBytes || program.Memsz > bootScratchBytes-program.Vaddr {
			return 0, 0, fmt.Errorf("live bootfs: seed0boot ELF load window exceeds %d-byte boot scratch", bootScratchBytes)
		}
		end := program.Vaddr + program.Memsz
		if end > maxEnd {
			maxEnd = end
		}
	}
	if maxEnd == 0 {
		return 0, 0, fmt.Errorf("live bootfs: seed0boot ELF has no loadable segments")
	}
	return info.Size(), int64((maxEnd + 4095) &^ 4095), nil
}

func fitsBootScratch(initBytes, initLoadBytes, archiveBytes int64) bool {
	if initBytes <= 0 || initLoadBytes <= 0 || archiveBytes < 0 ||
		initBytes > bootScratchBytes || initLoadBytes > bootScratchBytes || archiveBytes > maxBootfsBytes {
		return false
	}
	align8 := func(n int64) int64 { return (n + 7) &^ 7 }
	// The address list is kept until the init VMO has copied all staged pages.
	pages := (archiveBytes + 4095) / 4096
	metadataBytes := pages * 8
	return align8(initBytes)+align8(metadataBytes) <= bootScratchBytes &&
		align8(initBytes)+align8(initLoadBytes) <= bootScratchBytes
}

func validateELFClosure(workspace *config.Workspace, spec Spec, paths []string, available map[string]bool) error {
	for _, destination := range paths {
		app, _ := workspace.App(spec.Files[destination])
		filename := workspace.ArtifactPath(app)
		file, err := os.Open(filename)
		if err != nil {
			return err
		}
		var magic [4]byte
		_, readErr := io.ReadFull(file, magic[:])
		_ = file.Close()
		if readErr != nil || magic != [4]byte{0x7f, 'E', 'L', 'F'} {
			continue
		}
		image, err := elf.Open(filename)
		if err != nil {
			return fmt.Errorf("live bootfs: parse %s: %w", destination, err)
		}
		interpreter := ""
		for _, segment := range image.Progs {
			if segment.Type != elf.PT_INTERP {
				continue
			}
			data, err := io.ReadAll(segment.Open())
			if err != nil {
				_ = image.Close()
				return fmt.Errorf("live bootfs: read interpreter of %s: %w", destination, err)
			}
			interpreter = strings.TrimRight(string(data), "\x00")
		}
		needed, err := image.ImportedLibraries()
		_ = image.Close()
		if err != nil {
			return fmt.Errorf("live bootfs: read dependencies of %s: %w", destination, err)
		}
		if err := requireELFDependencies(destination, interpreter, needed, available); err != nil {
			return err
		}
	}
	return nil
}

func requireELFDependencies(destination, interpreter string, needed []string, available map[string]bool) error {
	if interpreter != "" && !available[interpreter] {
		return fmt.Errorf("live bootfs: %s needs missing interpreter %s", destination, interpreter)
	}
	for _, library := range needed {
		if !available["/lib/"+library] && !available["/lib/linux/"+library] {
			return fmt.Errorf("live bootfs: %s needs missing library %s", destination, library)
		}
	}
	return nil
}

func writeIfChanged(filename string, data []byte) error {
	if old, err := os.ReadFile(filename); err == nil && bytes.Equal(old, data) {
		return nil
	}
	temp, err := os.CreateTemp(filepath.Dir(filename), ".live-bootfs-manifest-*")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name())
	if _, err := temp.Write(data); err != nil {
		_ = temp.Close()
		return err
	}
	if err := temp.Close(); err != nil {
		return err
	}
	return os.Rename(temp.Name(), filename)
}
