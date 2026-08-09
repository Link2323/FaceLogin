package internal

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRemoveAllOrScheduleRebootRemovesWritableTreeImmediately(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "log")
	if err := os.MkdirAll(filepath.Join(dir, "nested"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "nested", "service.log"), []byte("test"), 0o644); err != nil {
		t.Fatal(err)
	}

	pending, err := removeAllOrScheduleReboot(dir)
	if err != nil {
		t.Fatalf("removeAllOrScheduleReboot returned error: %v", err)
	}
	if pending {
		t.Fatal("writable tree unexpectedly required a reboot")
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("directory still exists after immediate removal: %v", err)
	}
}

func TestUninstallCleanupRemovesObservedLegacyResidue(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "FaceLogin")
	if err := os.MkdirAll(filepath.Join(dir, "models"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(dir, "log"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "PadCalibration.exe"), []byte("legacy"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "log", "credential_provider.log"), []byte("log"), 0o644); err != nil {
		t.Fatal(err)
	}

	previousFS := EmbeddedFS
	EmbeddedFS = nil // exercise the explicit fallback manifest
	t.Cleanup(func() { EmbeddedFS = previousFS })

	removedFiles, filesPending, err := RemoveInstalledFiles(dir, true)
	if err != nil {
		t.Fatalf("RemoveInstalledFiles returned error: %v", err)
	}
	if removedFiles != 2 || filesPending {
		t.Fatalf("unexpected file-removal state: removed=%d pending=%v", removedFiles, filesPending)
	}

	removed, pending, err := RemoveInstalledDir(dir)
	if err != nil {
		t.Fatalf("RemoveInstalledDir returned error: %v", err)
	}
	if !removed || pending {
		t.Fatalf("unexpected removal state: removed=%v pending=%v", removed, pending)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("install directory still exists: %v", err)
	}
}

func TestRemoveInstalledDirReportsUnknownRootFile(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "FaceLogin")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	unknown := filepath.Join(dir, "keep-me.txt")
	if err := os.WriteFile(unknown, []byte("user file"), 0o644); err != nil {
		t.Fatal(err)
	}

	removed, pending, err := RemoveInstalledDir(dir)
	if err == nil || !strings.Contains(err.Error(), "unknown file") {
		t.Fatalf("expected an explicit unknown-file error, got %v", err)
	}
	if removed || pending {
		t.Fatalf("unexpected removal state: removed=%v pending=%v", removed, pending)
	}
	if _, err := os.Stat(unknown); err != nil {
		t.Fatalf("unknown file should have been preserved: %v", err)
	}
}
