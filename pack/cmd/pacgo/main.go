package main

import (
	"os"

	"capabilityos/pack/internal/cli"
)

func main() {
	os.Exit(cli.Execute())
}
