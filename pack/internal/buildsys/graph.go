package buildsys

import (
	"fmt"
	"sort"
	"strings"

	"capabilityos/pack/internal/config"
)

func RecipeGraphText(workspace *config.Workspace, recipeID string) (string, error) {
	graph := workspace.RecipeGraph()
	var roots []string
	if recipeID != "" {
		if _, ok := graph[recipeID]; !ok {
			return "", fmt.Errorf("recipe not found: %s", recipeID)
		}
		roots = []string{recipeID}
	} else {
		dependedOn := make(map[string]bool)
		for _, needs := range graph {
			for _, need := range needs {
				dependedOn[need] = true
			}
		}
		for id := range graph {
			if !dependedOn[id] {
				roots = append(roots, id)
			}
		}
		if len(roots) == 0 {
			for id := range graph {
				roots = append(roots, id)
			}
		}
		sort.Strings(roots)
	}
	var output strings.Builder
	var render func(string, string, map[string]bool)
	render = func(id, indent string, ancestors map[string]bool) {
		output.WriteString(indent)
		output.WriteString(id)
		output.WriteByte('\n')
		if ancestors[id] {
			return
		}
		next := make(map[string]bool, len(ancestors)+1)
		for key, value := range ancestors {
			next[key] = value
		}
		next[id] = true
		for _, need := range graph[id] {
			render(need, indent+"  ", next)
		}
	}
	for _, root := range roots {
		render(root, "", nil)
	}
	return output.String(), nil
}
