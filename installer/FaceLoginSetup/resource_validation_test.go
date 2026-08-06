package main

import (
	"testing"

	"FaceLoginSetup/internal"
)

func TestEmbeddedModelsMatchReleaseManifest(t *testing.T) {
	internal.EmbeddedFS = resources
	if err := internal.ValidateEmbeddedResources(); err != nil {
		t.Fatalf("embedded model validation failed: %v", err)
	}
}
