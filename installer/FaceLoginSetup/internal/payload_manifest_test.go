package internal

import (
	"slices"
	"sort"
	"testing"
)

// The payload manifest (payload_manifest.txt) is the single allow-list;
// requiredModels is the security manifest (name + size + SHA-256 per model).
// They must agree on the model set, so a half-updated allow-list can never
// ship or delete a model the other side doesn't know about.
func TestPayloadManifestMatchesRequiredModels(t *testing.T) {
	manifest := slices.Clone(PayloadManifestModelFiles())
	sort.Strings(manifest)
	required := make([]string, 0, len(requiredModels))
	for _, m := range requiredModels {
		required = append(required, m.fileName)
	}
	sort.Strings(required)
	if len(manifest) == 0 {
		t.Fatal("payload manifest has no models/ entries")
	}
	if !slices.Equal(manifest, required) {
		t.Errorf("payload manifest models != requiredModels names:\n manifest: %v\n required: %v",
			manifest, required)
	}

	root := PayloadManifestRootFiles()
	if len(root) == 0 {
		t.Fatal("payload manifest has no root entries")
	}
	// Every shipped executable must be uninstallable by name via the
	// fallback manifest — a root entry list that lost the uninstaller or the
	// binaries would strand files on uninstall.
	for _, must := range []string{"FaceLoginService.exe", "uninstall.exe"} {
		if !slices.Contains(root, must) {
			t.Errorf("payload manifest root missing %s", must)
		}
	}
}
