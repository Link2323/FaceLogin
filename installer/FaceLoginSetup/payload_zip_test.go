package main

import (
	"bytes"
	"io/fs"
	"os"
	"path/filepath"
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
	// The same complete legal texts must accompany the binaries and be readable
	// after installation, rather than remaining only in the source repository.
	for name, source := range map[string]string{
		"LICENSE.txt":             "../../LICENSE",
		"THIRD_PARTY_NOTICES.txt": "../../THIRD_PARTY_NOTICES.txt",
		"MODEL_LICENSES.md":       "../../third_party/MODEL_LICENSES.md",
	} {
		want, err := os.ReadFile(source)
		if err != nil {
			t.Fatal(err)
		}
		got, err := os.ReadFile(filepath.Join(dest, name))
		if err != nil {
			t.Fatalf("installed legal document %s missing: %v", name, err)
		}
		if len(got) == 0 || !bytes.Equal(got, want) {
			t.Errorf("installed legal document %s does not match its canonical source", name)
		}
	}
}
