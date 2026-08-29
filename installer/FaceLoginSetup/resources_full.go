//go:build !slim

package main

import "embed"

// resources embeds the full deployment payload (~95 MB): product binaries,
// runtime DLLs, models and the slim uninstaller copy. Only the default
// (full) build includes it.
//
//go:embed all:resources
//go:embed resources/models/det_10g_gnkps.onnx
//go:embed resources/models/w600k_r50.onnx
//go:embed resources/models/MiniFASNetV2.onnx
//go:embed resources/models/MiniFASNetV1SE.onnx
var resources embed.FS

// uninstallerBuild marks the slim build (wails build -tags slim): a ~9 MB
// uninstaller-only executable with no embedded payload. The uninstall flow
// then enumerates deletions from the static manifest in internal/extract.go
// instead of the embedded FS.
const uninstallerBuild = false
