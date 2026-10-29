package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func loadConfigText(t *testing.T, body string) (*Workspace, error) {
	t.Helper()
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "pack"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, PackFile), []byte("schema: 1\n"+body), 0o644); err != nil {
		t.Fatal(err)
	}
	return Load(root)
}

func TestRejectsObsoleteFileRebuild(t *testing.T) {
	_, err := loadConfigText(t, `apps:
  old:
    file: {path: out, rebuild: [sh, build.sh], input: [src]}
`)
	if err == nil || !strings.Contains(err.Error(), "obsolete file.rebuild") {
		t.Fatalf("error = %v", err)
	}
}

func TestRejectsAppPathOutsideRecipeOutputs(t *testing.T) {
	_, err := loadConfigText(t, `recipes:
  build: {run: [sh, build.sh], outputs: [owned]}
apps:
  bad: {file: {recipe: build, path: elsewhere/file}}
`)
	if err == nil || !strings.Contains(err.Error(), "outside recipe build outputs") {
		t.Fatalf("error = %v", err)
	}
}

func TestRejectsOverlappingRecipeOutputs(t *testing.T) {
	_, err := loadConfigText(t, `recipes:
  one: {run: [sh, one.sh], outputs: [out]}
  two: {run: [sh, two.sh], outputs: [out/child]}
apps: {}
`)
	if err == nil || !strings.Contains(err.Error(), "overlapping outputs") {
		t.Fatalf("error = %v", err)
	}
}

func TestRejectsUnknownRecipeKey(t *testing.T) {
	_, err := loadConfigText(t, `recipes:
  build: {run: [sh, build.sh], outputs: [out], surprise: true}
apps: {}
`)
	if err == nil || !strings.Contains(err.Error(), "unknown recipe key") {
		t.Fatalf("error = %v", err)
	}
}

func TestRecipeInputExcludeRequiresDiscovery(t *testing.T) {
	_, err := loadConfigText(t, `recipes:
  bad:
    run: ["true"]
    inputs: [{path: src, exclude: ["*.c"]}]
    outputs: [out]
apps: {}
`)
	if err == nil || !strings.Contains(err.Error(), "uses exclude without discover") {
		t.Fatalf("error = %v", err)
	}
}

func TestKbuildDirectoryFileDiscoveryIsAccepted(t *testing.T) {
	_, err := loadConfigText(t, `recipes:
  app:
    run: [true]
    inputs: [{path: src, exclude: ["*.c"]}]
    discover: [{kind: kbuild-dir-file, dir: .artifacts/output/provider-kbuild-dir}]
    outputs: [.artifacts/output]
apps: {}
`)
	if err != nil {
		t.Fatal(err)
	}
}

func TestRejectsRecipeNeedCycle(t *testing.T) {
	_, err := loadConfigText(t, `recipes:
  one: {run: [sh, one.sh], needs: [two], outputs: [one.out]}
  two: {run: [sh, two.sh], needs: [one], outputs: [two.out]}
apps: {}
`)
	if err == nil || !strings.Contains(err.Error(), "dependency cycle") {
		t.Fatalf("error = %v", err)
	}
}

func TestRejectsUnknownRecipeNeed(t *testing.T) {
	_, err := loadConfigText(t, `recipes:
  one: {run: [sh, one.sh], needs: [missing], outputs: [one.out]}
apps: {}
`)
	if err == nil || !strings.Contains(err.Error(), "needs unknown recipe missing") {
		t.Fatalf("error = %v", err)
	}
}

func TestDerivesRecipeNeedFromOwnedInput(t *testing.T) {
	workspace, err := loadConfigText(t, `recipes:
  producer: {run: [sh, producer.sh], outputs: [generated/tree]}
  consumer: {run: [sh, consumer.sh], inputs: [generated/tree/file], outputs: [consumer.out]}
apps: {}
`)
	if err != nil {
		t.Fatal(err)
	}
	consumer, _ := workspace.Recipe("consumer")
	if len(consumer.Needs) != 1 || consumer.Needs[0] != "producer" {
		t.Fatalf("derived needs = %#v", consumer.Needs)
	}
}
