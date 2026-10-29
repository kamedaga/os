package buildsys

import (
	"os"
	"testing"
)

// Recipes run in the devShell environment. Tests use an empty devShell delta,
// so they run in the test process's own environment without `nix develop`.
func TestMain(m *testing.M) {
	captureNixEnvironment = func(string) (nixEnvironmentDelta, error) {
		return nixEnvironmentDelta{}, nil
	}
	os.Exit(m.Run())
}
