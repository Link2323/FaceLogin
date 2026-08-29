//go:build !slim

package main

import (
	"archive/zip"
	"bytes"
	_ "embed"
	"io/fs"
)

// payloadZip is the compressed deployment payload (~44 MB deflated vs ~91 MB
// raw — go:embed stores files uncompressed). Produced by
// scripts/make_payload.ps1 from resources/; the zip entries keep the
// "resources/..." path prefix.
//
//go:embed payload.zip
var payloadZip []byte

// resources exposes the payload as an fs.FS. *zip.Reader implements fs.FS,
// so ValidateEmbeddedResources, ExtractAll, ExtractResource and the uninstall
// enumeration all work against it without path changes. Entry CRC32s are
// verified on every read; the model SHA-256 chain still applies on top.
var resources fs.FS = func() fs.FS {
	zr, err := zip.NewReader(bytes.NewReader(payloadZip), int64(len(payloadZip)))
	if err != nil {
		// The zip is embedded at build time; unreadable means a broken
		// build, not a runtime condition.
		panic("embedded payload.zip is not readable: " + err.Error())
	}
	return zr
}()

// uninstallerBuild marks the slim build (wails build -tags slim): a ~11 MB
// uninstaller-only executable with no embedded payload. The uninstall flow
// then enumerates deletions from the static manifest in internal/extract.go
// instead of the embedded FS.
const uninstallerBuild = false
