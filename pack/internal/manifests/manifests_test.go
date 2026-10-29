package manifests

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
	"time"

	"capabilityos/pack/internal/config"
)

func TestManifestContentKeyTracksExecutableIdentity(t *testing.T) {
	workspace := manifestKeyTestWorkspace(t, nil)
	original := manifestExecutableIdentity
	t.Cleanup(func() { manifestExecutableIdentity = original })

	manifestExecutableIdentity = func() ([]byte, error) { return []byte("first executable"), nil }
	_, first, err := manifestInputSnapshot(workspace, nil)
	if err != nil {
		t.Fatal(err)
	}
	manifestExecutableIdentity = func() ([]byte, error) { return []byte("second executable"), nil }
	_, second, err := manifestInputSnapshot(workspace, nil)
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(first, second) {
		t.Fatal("executable identity did not change the manifest content key")
	}
}

func TestManifestContentKeyTracksFileToDirectorySwitch(t *testing.T) {
	app := config.App{ID: "payload", File: "source", Rootfs: "/payload"}
	workspace := manifestKeyTestWorkspace(t, []config.App{app})
	artifact := workspace.ArtifactPath(app)
	if err := os.MkdirAll(filepath.Dir(artifact), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(artifact, []byte("file"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, before, err := manifestInputSnapshot(workspace, []config.App{app})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(artifact); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(artifact, 0o755); err != nil {
		t.Fatal(err)
	}
	_, after, err := manifestInputSnapshot(workspace, []config.App{app})
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(before, after) {
		t.Fatal("file-to-directory switch did not change the manifest content key")
	}
}

func TestManifestContentKeyTracksMissingToPresentSwitch(t *testing.T) {
	app := config.App{ID: "payload", File: "source", Bootfs: "/payload"}
	workspace := manifestKeyTestWorkspace(t, []config.App{app})
	_, before, err := manifestInputSnapshot(workspace, []config.App{app})
	if err != nil {
		t.Fatal(err)
	}
	artifact := workspace.ArtifactPath(app)
	if err := os.MkdirAll(filepath.Dir(artifact), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(artifact, []byte("present"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, after, err := manifestInputSnapshot(workspace, []config.App{app})
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Equal(before, after) {
		t.Fatal("missing-to-present switch did not change the manifest content key")
	}
}

func manifestKeyTestWorkspace(t *testing.T, apps []config.App) *config.Workspace {
	t.Helper()
	root := t.TempDir()
	configPath := filepath.Join(root, "pack.yaml")
	if err := os.WriteFile(configPath, []byte("config contents\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	appsMap := make(map[string]config.App, len(apps))
	for _, app := range apps {
		appsMap[app.ID] = app
	}
	return &config.Workspace{
		Root:       root,
		ConfigPath: configPath,
		Artifacts:  ".artifacts",
		State:      ".state",
		AppsMap:    appsMap,
	}
}

func TestManifestContentKeyTracksConfigAndDirectoryEntries(t *testing.T) {
	root := t.TempDir()
	configPath := filepath.Join(root, "pack.yaml")
	if err := os.WriteFile(configPath, []byte("config contents\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	source := filepath.Join(root, "fixture")
	if err := os.MkdirAll(source, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, "one"), []byte("one"), 0o644); err != nil {
		t.Fatal(err)
	}
	workspace := &config.Workspace{
		Root:       root,
		ConfigPath: configPath,
		Artifacts:  ".artifacts",
		State:      ".state",
		Manifests: config.Manifests{
			Bootfs:  ".artifacts/manifests/bootfs.generated.txt",
			Rootfs:  ".artifacts/manifests/rootfs.generated.txt",
			Startup: ".artifacts/manifests/startup.generated.txt",
		},
		AppsMap: map[string]config.App{
			"tree": {ID: "tree", File: "fixture", Rootfs: "/opt/tree"},
		},
	}
	artifact := workspace.ArtifactPath(workspace.AppsMap["tree"])
	if err := os.MkdirAll(filepath.Dir(artifact), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(source, artifact); err != nil {
		t.Fatal(err)
	}

	if _, err := Generate(workspace); err != nil {
		t.Fatal(err)
	}
	if _, fresh, err := ExistingIfFresh(workspace); err != nil || !fresh {
		t.Fatalf("fresh after generation = %v, err = %v", fresh, err)
	}
	if err := os.WriteFile(filepath.Join(source, "one"), []byte("different contents"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, fresh, err := ExistingIfFresh(workspace); err != nil || !fresh {
		t.Fatalf("fresh after directory file content change = %v, err = %v", fresh, err)
	}

	if err := os.WriteFile(filepath.Join(source, "two"), []byte("two"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, fresh, err := ExistingIfFresh(workspace); err != nil || fresh {
		t.Fatalf("fresh after adding directory entry = %v, err = %v", fresh, err)
	}
	if _, err := Generate(workspace); err != nil {
		t.Fatal(err)
	}

	future := time.Now().Add(24 * time.Hour)
	if err := os.Chtimes(configPath, future, future); err != nil {
		t.Fatal(err)
	}
	if _, fresh, err := ExistingIfFresh(workspace); err != nil || !fresh {
		t.Fatalf("fresh after config mtime-only change = %v, err = %v", fresh, err)
	}

	if err := os.WriteFile(configPath, []byte("different config contents\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, fresh, err := ExistingIfFresh(workspace); err != nil || fresh {
		t.Fatalf("fresh after config content change = %v, err = %v", fresh, err)
	}
}
