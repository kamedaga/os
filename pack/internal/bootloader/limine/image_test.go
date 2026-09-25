package limine

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLiveConfigHasOnlyNormalAndManualDiagnosticEntries(t *testing.T) {
	path := filepath.Join(t.TempDir(), "limine.conf")
	if err := writeConfig(path, true); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	for _, want := range []string{
		"default_entry: 1", "path: boot():/KERNEL.ELF",
		"path: boot():/DIAGBOOT.ELF",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("live config missing %q", want)
		}
	}
	if strings.Count(text, "    protocol: limine") != 2 {
		t.Fatalf("expected exactly two boot entries:\n%s", text)
	}
}

func TestOrdinaryConfigHasNoDiagnosticEntry(t *testing.T) {
	path := filepath.Join(t.TempDir(), "limine.conf")
	if err := writeConfig(path, false); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(data), "DIAGBOOT.ELF") {
		t.Fatalf("ordinary config unexpectedly includes diagnostic entry:\n%s", data)
	}
}
