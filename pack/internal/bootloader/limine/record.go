package limine

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sort"

	"capabilityos/pack/internal/buildsys"
	"capabilityos/pack/internal/config"
)

// imageRecord names every byte source of an ordinary Limine image. An image
// is reused only when all inputs and the image itself still match.
type imageRecord struct {
	Inputs map[string]string `json:"inputs"`
	Image  string            `json:"image"`
}

func recordPath(imagePath string) string {
	return imagePath + ".inputs.json"
}

func imageInputDigests(workspace *config.Workspace, inputs Inputs, limine, dataDir string) (map[string]string, error) {
	files := map[string]string{
		"KERNEL.ELF":      inputs.KernelELF,
		"INITAPP.ELF":     inputs.InitELF,
		"BOOTFS.IMG":      inputs.BootfsImage,
		"limine-bios.sys": filepath.Join(dataDir, "limine-bios.sys"),
		"BOOTX64.EFI":     filepath.Join(dataDir, "BOOTX64.EFI"),
		"limine":          limine,
	}
	if inputs.DiagnosticELF != "" {
		files["DIAGBOOT.ELF"] = inputs.DiagnosticELF
	}
	digests := make(map[string]string, len(files)+1)
	for name, path := range files {
		digest, err := buildsys.FileDigest(workspace, path)
		if err != nil {
			return nil, err
		}
		digests[name] = hex.EncodeToString(digest)
	}
	digests["limine.conf"] = hex.EncodeToString(sha256Bytes([]byte(configContent(inputs.DiagnosticELF != ""))))
	return digests, nil
}

func readImageRecord(imagePath string) (imageRecord, bool) {
	data, err := os.ReadFile(recordPath(imagePath))
	if err != nil {
		return imageRecord{}, false
	}
	var record imageRecord
	if json.Unmarshal(data, &record) != nil || record.Inputs == nil {
		return imageRecord{}, false
	}
	return record, true
}

func writeImageRecord(workspace *config.Workspace, imagePath string, inputs map[string]string) error {
	digest, err := buildsys.FileDigest(workspace, imagePath)
	if err != nil {
		return err
	}
	data, err := json.MarshalIndent(imageRecord{Inputs: inputs, Image: hex.EncodeToString(digest)}, "", "  ")
	if err != nil {
		return err
	}
	tmp := recordPath(imagePath) + ".tmp"
	if err := os.WriteFile(tmp, append(data, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, recordPath(imagePath))
}

func removeImageRecord(imagePath string) error {
	err := os.Remove(recordPath(imagePath))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	return err
}

// imageMatchesRecord reports whether imagePath is byte-identical to the image
// last built from exactly these inputs.
func imageMatchesRecord(workspace *config.Workspace, imagePath string, inputs map[string]string) (bool, error) {
	record, ok := readImageRecord(imagePath)
	if !ok || !sameDigests(record.Inputs, inputs) {
		return false, nil
	}
	digest, err := buildsys.FileDigest(workspace, imagePath)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return hex.EncodeToString(digest) == record.Image, nil
}

func sameDigests(lhs, rhs map[string]string) bool {
	if len(lhs) != len(rhs) {
		return false
	}
	for name, digest := range lhs {
		if rhs[name] != digest {
			return false
		}
	}
	return true
}

// ImageFreshness compares the boot files recorded for imagePath with the
// files that would be packed now. It never builds; an empty result means the
// image carries the current kernel, init and bootfs.
func ImageFreshness(workspace *config.Workspace, imagePath string, inputs Inputs) ([]string, error) {
	record, ok := readImageRecord(imagePath)
	if !ok {
		if _, err := os.Stat(imagePath); errors.Is(err, os.ErrNotExist) {
			return []string{"limine image missing: " + workspace.Rel(imagePath)}, nil
		}
		return []string{"no input record for " + workspace.Rel(imagePath)}, nil
	}
	current := map[string]string{
		"KERNEL.ELF":  inputs.KernelELF,
		"INITAPP.ELF": inputs.InitELF,
		"BOOTFS.IMG":  inputs.BootfsImage,
	}
	var reasons []string
	for name, path := range current {
		digest, err := buildsys.FileDigest(workspace, path)
		if errors.Is(err, os.ErrNotExist) {
			reasons = append(reasons, name+" source missing: "+workspace.Rel(path))
			continue
		}
		if err != nil {
			return nil, err
		}
		if hex.EncodeToString(digest) != record.Inputs[name] {
			reasons = append(reasons, "limine image carries an older "+name)
		}
	}
	digest, err := buildsys.FileDigest(workspace, imagePath)
	if err != nil {
		return nil, err
	}
	if hex.EncodeToString(digest) != record.Image {
		reasons = append(reasons, "limine image modified after pacgo built it")
	}
	sort.Strings(reasons)
	return reasons, nil
}
