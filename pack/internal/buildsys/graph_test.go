package buildsys

import (
	"strings"
	"testing"

	"capabilityos/pack/internal/config"
)

func TestRecipeGraphText(t *testing.T) {
	workspace := &config.Workspace{RecipesMap: map[string]config.Recipe{
		"base":  {ID: "base"},
		"left":  {ID: "left", Needs: []string{"base"}},
		"right": {ID: "right", Needs: []string{"base"}},
		"top":   {ID: "top", Needs: []string{"right", "left"}},
	}}
	graph, err := RecipeGraphText(workspace, "top")
	if err != nil {
		t.Fatal(err)
	}
	want := "top\n  left\n    base\n  right\n    base\n"
	if graph != want {
		t.Fatalf("graph:\n%s\nwant:\n%s", graph, want)
	}
	if _, err := RecipeGraphText(workspace, "missing"); err == nil || !strings.Contains(err.Error(), "recipe not found") {
		t.Fatalf("missing recipe error = %v", err)
	}
}
