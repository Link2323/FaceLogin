package main

import (
	"io/fs"
	"testing"

	"FaceLoginSetup/internal"
)

// Locks the payload.zip contract: the embedded zip must expose the same
// fs.FS view as the old uncompressed embed (explicit directory entries with
// the "resources/..." prefix — scripts/make_payload.ps1 uses .NET ZipFile
// precisely because Compress-Archive writes no directory entries and would
// break ReadDir), and extracting it must yield models whose SHA-256s match
// the calibration-critical hashes.
func TestPayloadZipExtractionChain(t *testing.T) {
	internal.EmbeddedFS = resources

	entries, err := fs.ReadDir(resources, "resources")
	if err != nil {
		t.Fatalf("ReadDir resources: %v", err)
	}
	files := 0
	hasModelsDir := false
	for _, e := range entries {
		if e.IsDir() {
			hasModelsDir = hasModelsDir || e.Name() == "models"
			continue
		}
		files++
	}
	if files < 9 { // 3 product binaries + uninstall.exe + 5 runtime DLLs
		t.Errorf("expected at least 9 root payload files, got %d", files)
	}
	if !hasModelsDir {
		t.Error("resources/models directory not enumerable — zip lost its directory entries?")
	}

	models, err := fs.ReadDir(resources, "resources/models")
	if err != nil {
		t.Fatalf("ReadDir resources/models: %v", err)
	}
	if len(models) != 4 {
		t.Errorf("expected 4 models, got %d", len(models))
	}

	dest := t.TempDir()
	if err := internal.ExtractAll(dest, nil); err != nil {
		t.Fatalf("ExtractAll: %v", err)
	}
	if err := internal.ValidateInstalledModels(dest); err != nil {
		t.Errorf("installed models failed SHA-256 after extraction: %v", err)
	}
}
