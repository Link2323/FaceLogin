package internal

import (
	_ "embed"
	"strings"
)

// payloadManifestTxt is the single payload allow-list, shared with
// scripts/build_installer.ps1 (which reads this file from disk) — see
// payload_manifest.txt itself for the format.
//
//go:embed payload_manifest.txt
var payloadManifestTxt string

// parsePayloadManifest splits the manifest into root files and model names
// ("models/<file>" entries). Malformed entries fail loudly at startup: a
// typo here silently ships or silently deletes the wrong file.
func parsePayloadManifest() (rootFiles, modelFiles []string) {
	for _, raw := range strings.Split(payloadManifestTxt, "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if name, ok := strings.CutPrefix(line, "models/"); ok {
			if name == "" || strings.ContainsAny(name, "/\\") {
				panic("payload manifest: bad models/ entry: " + line)
			}
			modelFiles = append(modelFiles, name)
			continue
		}
		if line == "" || strings.ContainsAny(line, "/\\") {
			panic("payload manifest: entry outside root/ or models/: " + line)
		}
		rootFiles = append(rootFiles, line)
	}
	return rootFiles, modelFiles
}

// PayloadManifestRootFiles lists every allow-listed root payload file.
func PayloadManifestRootFiles() []string {
	root, _ := parsePayloadManifest()
	return root
}

// PayloadManifestModelFiles lists every allow-listed model file name.
func PayloadManifestModelFiles() []string {
	_, models := parsePayloadManifest()
	return models
}
