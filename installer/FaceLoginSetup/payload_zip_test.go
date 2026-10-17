//go:build !slim

package main

import (
	"bytes"
	"io/fs"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"sort"
	"testing"

	"FaceLoginSetup/internal"
)

// Locks the payload.zip contract: the embedded zip must expose the same
// fs.FS view as the old uncompressed embed (explicit directory entries with
// the "resources/..." prefix — scripts/make_payload.ps1 uses .NET ZipFile
// precisely because Compress-Archive writes no directory entries and would
// break ReadDir), and its contents must match the payload allow-list
// (internal/payload_manifest.txt) file for file — the same list the
// uninstaller's static manifest is derived from.
func TestPayloadZipExtractionChain(t *testing.T) {
	internal.EmbeddedFS = resources

	entries, err := fs.ReadDir(resources, "resources")
	if err != nil {
		t.Fatalf("ReadDir resources: %v", err)
	}
	hasModelsDir := false
	var zipRoot []string
	for _, e := range entries {
		if e.IsDir() {
			hasModelsDir = hasModelsDir || e.Name() == "models"
			continue
		}
		zipRoot = append(zipRoot, e.Name())
	}
	if !hasModelsDir {
		t.Error("resources/models directory not enumerable — zip lost its directory entries?")
	}

	models, err := fs.ReadDir(resources, "resources/models")
	if err != nil {
		t.Fatalf("ReadDir resources/models: %v", err)
	}
	var zipModels []string
	for _, e := range models {
		if !e.IsDir() {
			zipModels = append(zipModels, e.Name())
		}
	}

	// Zip contents ↔ allow-list equality: an entry on either side alone
	// means a stale zip or a half-updated manifest.
	sort.Strings(zipRoot)
	wantRoot := slices.Clone(internal.PayloadManifestRootFiles())
	sort.Strings(wantRoot)
	if !reflect.DeepEqual(zipRoot, wantRoot) {
		t.Errorf("payload.zip root files != payload manifest:\n zip:    %v\n manifest: %v",
			zipRoot, wantRoot)
	}
	sort.Strings(zipModels)
	wantModels := slices.Clone(internal.PayloadManifestModelFiles())
	sort.Strings(wantModels)
	if !reflect.DeepEqual(zipModels, wantModels) {
		t.Errorf("payload.zip models != payload manifest:\n zip:    %v\n manifest: %v",
			zipModels, wantModels)
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
		"MODEL_LICENSES.txt":      "../../third_party/MODEL_LICENSES.txt",
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
