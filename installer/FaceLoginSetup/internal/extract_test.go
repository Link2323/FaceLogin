package internal

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"
)

func TestUninstallCleanupRemovesCameraTuneResidue(t *testing.T) {
	for _, slim := range []bool{true, false} {
		name := "full"
		if slim {
			name = "slim"
		}
		t.Run(name, func(t *testing.T) {
			dir := filepath.Join(t.TempDir(), "FaceLogin")
			if err := os.MkdirAll(filepath.Join(dir, "models"), 0o755); err != nil {
				t.Fatal(err)
			}
			state := filepath.Join(dir, "camera_tune.state")
			if err := os.WriteFile(state, []byte("exposure=-5\ngain=10\n"), 0o644); err != nil {
				t.Fatal(err)
			}
			previousFS := EmbeddedFS
			t.Cleanup(func() { EmbeddedFS = previousFS })
			EmbeddedFS = nil
			if !slim {
				EmbeddedFS = fstest.MapFS{"resources/LICENSE.txt": &fstest.MapFile{Data: []byte("license")}}
			}
			if _, pending, err := RemoveInstalledFiles(dir, true); err != nil || pending {
				t.Fatalf("file cleanup: pending=%v err=%v", pending, err)
			}
			if FileExists(state) {
				t.Error("camera_tune.state remains after full user-data removal")
			}
			removed, pending, err := RemoveInstalledDir(dir)
			if err != nil || !removed || pending {
				t.Fatalf("install directory remains: removed=%v pending=%v err=%v", removed, pending, err)
			}
			if _, err := os.Stat(dir); !os.IsNotExist(err) {
				t.Fatalf("install directory still exists: %v", err)
			}
		})
	}
}

func TestUpgradePreservesCameraTuneState(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "FaceLogin")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	state := filepath.Join(dir, "camera_tune.state")
	want := "exposure=-5\ngain=10\n"
	if err := os.WriteFile(state, []byte(want), 0o644); err != nil {
		t.Fatal(err)
	}
	previousFS := EmbeddedFS
	EmbeddedFS = nil
	t.Cleanup(func() { EmbeddedFS = previousFS })
	if _, pending, err := RemoveInstalledFiles(dir, false); err != nil || pending {
		t.Fatalf("upgrade cleanup: pending=%v err=%v", pending, err)
	}
	got, err := os.ReadFile(state)
	if err != nil || string(got) != want {
		t.Fatalf("upgrade must preserve camera tune state: err=%v", err)
	}
}

func TestFinalizeRecognizesOnlyCameraTuneResidue(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "FaceLogin")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "camera_tune.state"), []byte("gain=10\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if !IsSafeInstallDir(dir) {
		t.Fatal("known runtime residue must allow a follow-up uninstall")
	}
	removed, pending, err := RemoveInstalledDir(dir)
	if err != nil || !removed || pending {
		t.Fatalf("finalization must remove known state residue: removed=%v pending=%v err=%v", removed, pending, err)
	}
}

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
	for _, name := range []string{"LICENSE.txt", "THIRD_PARTY_NOTICES.txt", "MODEL_LICENSES.txt"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte("legal document"), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	previousFS := EmbeddedFS
	EmbeddedFS = nil // exercise the explicit fallback manifest
	t.Cleanup(func() { EmbeddedFS = previousFS })

	removedFiles, filesPending, err := RemoveInstalledFiles(dir, true)
	if err != nil {
		t.Fatalf("RemoveInstalledFiles returned error: %v", err)
	}
	if removedFiles != 5 || filesPending {
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
