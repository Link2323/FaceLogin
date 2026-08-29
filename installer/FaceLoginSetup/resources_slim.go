//go:build slim

package main

import "io/fs"

// resources is nil in the slim uninstaller build (no embedded payload); the
// uninstall flow then falls back to the static manifest in internal/extract.go.
var resources fs.FS

const uninstallerBuild = true
