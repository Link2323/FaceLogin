package internal

import (
	"crypto/sha256"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/sys/windows"
)

const (
	cleanupRetryAttempts = 10
	cleanupRetryDelay    = 200 * time.Millisecond
)

// EmbeddedFS is set by the caller (main.go's //go:embed resources/*).
// The caller must assign it before calling ExtractAll.
var EmbeddedFS fs.FS

type requiredModel struct {
	embeddedPath string
	fileName     string
	size         int64
	sha256       string
}

// These hashes identify the exact, normalized models the C++ pipeline was
// calibrated against.  Presence alone is insufficient: a truncated ONNX file
// or the unnormalized SCRFD export must stop installation before the existing
// service is touched.
var requiredModels = []requiredModel{
	{
		embeddedPath: "resources/models/det_10g_gnkps.onnx",
		fileName:     "det_10g_gnkps.onnx",
		size:         4257451,
		sha256:       "07b62718eb454ee1881465c12d0d0546f2e916e3bb549f142dc221729bf7f4dc",
	},
	{
		embeddedPath: "resources/models/w600k_r50.onnx",
		fileName:     "w600k_r50.onnx",
		size:         43805153,
		sha256:       "b9b2ea32afaa88dfd226255f354ea241c3a744abf75b3dbdcf00c95f7f00e185",
	},
	{
		embeddedPath: "resources/models/MiniFASNetV2.onnx",
		fileName:     "MiniFASNetV2.onnx",
		size:         1743581,
		sha256:       "b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907",
	},
	{
		embeddedPath: "resources/models/MiniFASNetV1SE.onnx",
		fileName:     "MiniFASNetV1SE.onnx",
		size:         1742335,
		sha256:       "ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676",
	},
}

// Root-level payload in the current installer. Keep this aligned with the
// allow-list in docs/BUILD.md; it is also the fallback removal manifest if the
// embedded filesystem cannot be enumerated during uninstall — which is the
// ONLY manifest the slim uninstaller build has (no embedded resources).
var currentRootPayloadFiles = []string{
	"FaceLoginService.exe",
	"FaceLoginCredentialProvider.dll",
	"FaceLoginConsole.exe",
	"uninstall.exe",
	"abseil_dll.dll",
	"libprotobuf-lite.dll",
	"libprotobuf.dll",
	"onnxruntime.dll",
	"re2.dll",
}

// Files deployed by earlier releases but intentionally absent from the current
// manifest. Upgrade and uninstall remove only these exact legacy names.
var legacyModelFiles = []string{
	"OULU_Protocol_2_model_0_0.onnx",
}

// Runtime DLLs shipped by v1.5 and earlier (when the pipeline still depended on
// dlib, which pulled in OpenBLAS/LAPACK via MinGW). v1.6 removed dlib and now
// ships a different runtime set (onnxruntime/abseil/re2/protobuf), so these
// older DLLs linger in the install dir of upgraded machines. Because they are
// not in the current manifest, RemoveInstalledFiles would never touch them,
// leaving RemoveInstalledDir to see a non-empty directory and (correctly)
// refuse to delete it — so the whole install folder survived uninstall. Listed
// here so they are swept away by name, exactly like legacyModelFiles.
var legacyRuntimeFiles = []string{
	"libgcc_s_seh-1.dll",
	"libgfortran-5.dll",
	"liblapack.dll",
	"libquadmath-0.dll",
	"libwinpthread-1.dll",
	"openblas.dll",
}

// Diagnostic executables were accidentally present in some older install
// directories. They are development-only tools and are never part of the
// current installer payload, but uninstall should remove them when upgrading
// from one of those releases.
var legacyToolFiles = []string{
	"PadCalibration.exe",
	"EmbeddingTest.exe",
	"CameraLifecycleTest.exe",
}

func validateModelReader(label string, r io.Reader, actualSize int64, model requiredModel) error {
	if actualSize != model.size {
		return fmt.Errorf("%s has size %d, expected %d", label, actualSize, model.size)
	}
	h := sha256.New()
	n, err := io.Copy(h, r)
	if err != nil {
		return fmt.Errorf("hash %s: %w", label, err)
	}
	if n != model.size {
		return fmt.Errorf("%s read %d bytes, expected %d", label, n, model.size)
	}
	actualHash := fmt.Sprintf("%x", h.Sum(nil))
	if !strings.EqualFold(actualHash, model.sha256) {
		return fmt.Errorf("%s SHA-256 %s, expected %s", label, actualHash, model.sha256)
	}
	return nil
}

// ValidateEmbeddedResources checks every security-critical model before the
// installer stops an existing service or mutates the target installation.
func ValidateEmbeddedResources() error {
	if EmbeddedFS == nil {
		return fmt.Errorf("embedded resource filesystem is not initialized")
	}
	for _, model := range requiredModels {
		f, err := EmbeddedFS.Open(model.embeddedPath)
		if err != nil {
			return fmt.Errorf("required model missing (%s): %w", model.fileName, err)
		}
		info, statErr := f.Stat()
		if statErr != nil {
			f.Close()
			return fmt.Errorf("stat embedded %s: %w", model.fileName, statErr)
		}
		if info.IsDir() {
			f.Close()
			return fmt.Errorf("required model is a directory: %s", model.fileName)
		}
		validateErr := validateModelReader(model.embeddedPath, f, info.Size(), model)
		closeErr := f.Close()
		if validateErr != nil {
			return validateErr
		}
		if closeErr != nil {
			return fmt.Errorf("close embedded %s: %w", model.fileName, closeErr)
		}
	}
	return nil
}

// ValidateInstalledModels verifies bytes on disk after extraction and before
// registering COM or starting the SYSTEM service.
func ValidateInstalledModels(destDir string) error {
	for _, model := range requiredModels {
		path := filepath.Join(destDir, "models", model.fileName)
		f, err := os.Open(path)
		if err != nil {
			return fmt.Errorf("open installed %s: %w", model.fileName, err)
		}
		info, statErr := f.Stat()
		if statErr != nil {
			f.Close()
			return fmt.Errorf("stat installed %s: %w", model.fileName, statErr)
		}
		validateErr := validateModelReader(path, f, info.Size(), model)
		closeErr := f.Close()
		if validateErr != nil {
			return validateErr
		}
		if closeErr != nil {
			return fmt.Errorf("close installed %s: %w", model.fileName, closeErr)
		}
	}
	return nil
}

// ExtractResource extracts a single embedded resource to a destination path.
func ExtractResource(embeddedPath, destPath string) error {
	data, err := fs.ReadFile(EmbeddedFS, embeddedPath)
	if err != nil {
		return fmt.Errorf("read embedded %s: %w", embeddedPath, err)
	}
	return os.WriteFile(destPath, data, 0644)
}

// ExtractAll extracts all embedded resources to destDir, organized:
//
//	destDir/
//	  FaceLoginService.exe
//	  FaceLoginCredentialProvider.dll
//	  FaceLoginConsole.exe
//	  openblas.dll  (etc.)
//	  models/
//	    *.dat, *.onnx  (model files)
func ExtractAll(destDir string, progressFn func(step, total int, name string)) error {
	// Create target directories
	modelsDir := filepath.Join(destDir, "models")
	if err := os.MkdirAll(modelsDir, 0755); err != nil {
		return fmt.Errorf("create models dir: %w", err)
	}

	// List embedded files (recursively from "resources" using all: embed)
	entries, err := fs.ReadDir(EmbeddedFS, "resources")
	if err != nil {
		return fmt.Errorf("read embedded resources: %w", err)
	}
	modelEntries, err := fs.ReadDir(EmbeddedFS, "resources/models")
	if err != nil {
		return fmt.Errorf("read embedded models: %w", err)
	}

	total := len(entries) + len(modelEntries)
	step := 0

	processEntry := func(name, srcPath, dstPath string) error {
		step++
		if progressFn != nil {
			progressFn(step, total, name)
		}
		return ExtractResource(srcPath, dstPath)
	}

	for _, entry := range entries {
		// Skip directories (handled separately)
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		srcPath := "resources/" + name

		// .dat files go to models/ subdirectory (historical convention)
		var dstPath string
		if filepath.Ext(name) == ".dat" {
			dstPath = filepath.Join(modelsDir, name)
		} else {
			dstPath = filepath.Join(destDir, name)
		}

		if err := processEntry(name, srcPath, dstPath); err != nil {
			return fmt.Errorf("extract %s: %w", name, err)
		}
	}

	// Extract model files from resources/models/ to dest/models/
	for _, entry := range modelEntries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		srcPath := "resources/models/" + name
		dstPath := filepath.Join(modelsDir, name)
		if err := processEntry(name, srcPath, dstPath); err != nil {
			return fmt.Errorf("extract %s: %w", name, err)
		}
	}
	for _, name := range legacyModelFiles {
		legacyPath := filepath.Join(modelsDir, name)
		if err := os.Remove(legacyPath); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("remove legacy model %s: %w", name, err)
		}
	}
	return nil
}

// scheduleDeleteOnReboot marks one file or directory for verified deletion at
// the next boot. Callers must propagate this error: treating a failed
// MoveFileEx as success is what previously made uninstall report a clean purge
// while files remained on disk.
func scheduleDeleteOnReboot(path string) error {
	ptr, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return fmt.Errorf("encode pending-delete path %s: %w", path, err)
	}
	if err := windows.MoveFileEx(ptr, nil, windows.MOVEFILE_DELAY_UNTIL_REBOOT); err != nil {
		return fmt.Errorf("schedule pending delete %s: %w", path, err)
	}
	return nil
}

// removeFileOrScheduleReboot removes one file immediately, or verifies that it
// was scheduled for deletion after reboot. A successful schedule is distinct
// from an immediate removal; a failed schedule is a real error.
func removeFileOrScheduleReboot(path string) (removed, rebootRequired bool, err error) {
	var removeErr error
	for attempt := 0; attempt < cleanupRetryAttempts; attempt++ {
		removeErr = os.Remove(path)
		if removeErr == nil {
			return true, false, nil
		}
		if os.IsNotExist(removeErr) {
			return false, false, nil
		}
		if attempt+1 < cleanupRetryAttempts {
			time.Sleep(cleanupRetryDelay)
		}
	}
	if scheduleErr := scheduleDeleteOnReboot(path); scheduleErr != nil {
		return false, false, fmt.Errorf("remove %s: %v; %w", path, removeErr, scheduleErr)
	}
	return false, true, nil
}

// removeAllOrScheduleReboot first attempts an immediate recursive removal. If
// Windows refuses an entry (normally because LogonUI still has it open), every
// remaining entry is either removed or explicitly scheduled. Traversal and
// scheduling failures are returned instead of being silently discarded.
func removeAllOrScheduleReboot(path string) (rebootRequired bool, err error) {
	for attempt := 0; attempt < cleanupRetryAttempts; attempt++ {
		if err := os.RemoveAll(path); err == nil {
			return false, nil
		}
		if attempt+1 < cleanupRetryAttempts {
			time.Sleep(cleanupRetryDelay)
		}
	}

	var dirs []string
	var firstErr error
	recordErr := func(err error) {
		if err != nil && firstErr == nil {
			firstErr = err
		}
	}

	walkErr := filepath.Walk(path, func(p string, info os.FileInfo, entryErr error) error {
		if entryErr != nil {
			recordErr(fmt.Errorf("walk %s: %w", p, entryErr))
			return nil
		}
		if info.IsDir() {
			dirs = append(dirs, p)
			return nil
		}
		_, pending, removeErr := removeFileOrScheduleReboot(p)
		if pending {
			rebootRequired = true
		}
		recordErr(removeErr)
		return nil
	})
	recordErr(walkErr)

	// Children were scheduled before these directories, so PendingFileRename
	// operations execute in an order that leaves each directory empty first.
	for i := len(dirs) - 1; i >= 0; i-- {
		dir := dirs[i]
		removeErr := os.Remove(dir)
		if removeErr == nil || os.IsNotExist(removeErr) {
			continue
		}
		if scheduleErr := scheduleDeleteOnReboot(dir); scheduleErr != nil {
			recordErr(fmt.Errorf("remove directory %s: %v; %w", dir, removeErr, scheduleErr))
		} else {
			rebootRequired = true
		}
	}
	return rebootRequired, firstErr
}

// RemoveInstalledFiles removes the current payload plus explicitly known
// legacy files. It reports immediate removals separately from verified
// pending-reboot removals, and never suppresses a real deletion error.
func RemoveInstalledFiles(destDir string, removeUserData bool) (removed int, rebootRequired bool, err error) {
	var firstErr error
	recordErr := func(err error) {
		if err != nil && firstErr == nil {
			firstErr = err
		}
	}
	removeKnownFile := func(path string) {
		if !FileExists(path) {
			return
		}
		removedNow, pending, removeErr := removeFileOrScheduleReboot(path)
		if removedNow {
			removed++
		}
		if pending {
			rebootRequired = true
		}
		recordErr(removeErr)
	}

	modelsDir := filepath.Join(destDir, "models")
	var entries []fs.DirEntry
	var readErr error
	if EmbeddedFS == nil {
		readErr = fmt.Errorf("embedded resource filesystem is not initialized")
	} else {
		entries, readErr = fs.ReadDir(EmbeddedFS, "resources")
	}
	if readErr != nil {
		for _, name := range currentRootPayloadFiles {
			removeKnownFile(filepath.Join(destDir, name))
		}
	} else {
		for _, entry := range entries {
			if entry.IsDir() {
				continue
			}
			name := entry.Name()
			dstPath := filepath.Join(destDir, name)
			if filepath.Ext(name) == ".dat" {
				dstPath = filepath.Join(modelsDir, name)
			}
			removeKnownFile(dstPath)
		}
	}

	// Production and legacy model names are static security manifests; do not
	// depend on a second embedded-filesystem enumeration that could fail silently.
	for _, model := range requiredModels {
		removeKnownFile(filepath.Join(modelsDir, model.fileName))
	}
	for _, name := range legacyModelFiles {
		removeKnownFile(filepath.Join(modelsDir, name))
	}
	for _, name := range legacyRuntimeFiles {
		removeKnownFile(filepath.Join(destDir, name))
	}
	for _, name := range legacyToolFiles {
		removeKnownFile(filepath.Join(destDir, name))
	}

	if removeUserData {
		for _, sub := range []string{"data", "log"} {
			dir := filepath.Join(destDir, sub)
			if !DirExists(dir) {
				continue
			}
			pending, removeErr := removeAllOrScheduleReboot(dir)
			if pending {
				rebootRequired = true
			}
			if removeErr != nil {
				recordErr(removeErr)
			} else if !pending {
				removed++
			}
		}
	}

	// The WebView2 runtime's legacy default user-data folder next to the
	// console EXE (predates the move to %LOCALAPPDATA%). Removed on every
	// uninstall/upgrade so the install dir can finalize cleanly; harmless if
	// absent.
	if wv2Dir := filepath.Join(destDir, "FaceLoginConsole.exe.WebView2"); DirExists(wv2Dir) {
		pending, removeErr := removeAllOrScheduleReboot(wv2Dir)
		if pending {
			rebootRequired = true
		}
		if removeErr != nil {
			recordErr(removeErr)
		}
	}

	return removed, rebootRequired, firstErr
}

func isKnownRootPayload(name string) bool {
	for _, names := range [][]string{currentRootPayloadFiles, legacyRuntimeFiles, legacyToolFiles} {
		for _, known := range names {
			if strings.EqualFold(name, known) {
				return true
			}
		}
	}
	return false
}

// isOwnedSubdir reports whether a subdirectory of the install dir belongs to
// the product and may be removed on uninstall. models/data/log are deployed
// by the installer; FaceLoginConsole.exe.WebView2 is created by the WebView2
// runtime next to the console EXE (its legacy default user-data-folder
// location) — without recognizing it, the dir looks unknown and the whole
// install folder is left behind.
func isOwnedSubdir(name string) bool {
	switch {
	case strings.EqualFold(name, "models"),
		strings.EqualFold(name, "data"),
		strings.EqualFold(name, "log"),
		strings.EqualFold(name, "FaceLoginConsole.exe.WebView2"):
		return true
	}
	return false
}

// RemoveInstalledDir finalizes the verified FaceLogin install directory. Only
// known root payload files and installer-owned subdirectories are touched;
// unknown entries produce an explicit error. If any child is pending deletion,
// the root directory is also verified as pending so no empty shell is left
// after reboot.
func RemoveInstalledDir(destDir string) (removed, rebootRequired bool, err error) {
	if !DirExists(destDir) {
		return true, false, nil
	}
	entries, err := os.ReadDir(destDir)
	if err != nil {
		return false, false, fmt.Errorf("read install directory %s: %w", destDir, err)
	}

	for _, entry := range entries {
		entryPath := filepath.Join(destDir, entry.Name())
		if entry.IsDir() {
			if !isOwnedSubdir(entry.Name()) {
				return false, rebootRequired, fmt.Errorf("install directory contains unknown subdirectory: %s", entry.Name())
			}
			pending, removeErr := removeAllOrScheduleReboot(entryPath)
			if removeErr != nil {
				return false, rebootRequired || pending, removeErr
			}
			if pending {
				rebootRequired = true
			}
			continue
		}

		if !isKnownRootPayload(entry.Name()) {
			return false, rebootRequired, fmt.Errorf("install directory contains unknown file: %s", entry.Name())
		}
		_, pending, removeErr := removeFileOrScheduleReboot(entryPath)
		if removeErr != nil {
			return false, rebootRequired || pending, removeErr
		}
		if pending {
			rebootRequired = true
		}
	}

	removeErr := os.Remove(destDir)
	if removeErr == nil || os.IsNotExist(removeErr) {
		return true, rebootRequired, nil
	}
	if !rebootRequired {
		return false, false, fmt.Errorf("remove install directory %s: %w", destDir, removeErr)
	}
	if scheduleErr := scheduleDeleteOnReboot(destDir); scheduleErr != nil {
		return false, true, fmt.Errorf("remove install directory %s: %v; %w", destDir, removeErr, scheduleErr)
	}
	return false, true, nil
}

// RemoveProgramData deletes the shared runtime-data directory
// (%ProgramData%\FaceLogin: config.json, the enrolled face database users.dat,
// logs, and the models cache). The installer points DataPath at the install
// directory, so this directory is only populated when an older release used it
// as the default or when the app was run standalone — but on such machines it
// accumulates real data that should not survive a full uninstall.
//
// The path is resolved from %ProgramData% rather than the registry because the
// uninstall flow deletes the registry key shortly after this call. A safety
// guard mirrors IsSafeInstallDir: the resolved directory's base name must be
// exactly "FaceLogin" (case-insensitive) before anything is removed, so a
// maliciously empty or corrupted %ProgramData% can never turn this into a
// recursive wipe of an arbitrary folder. Returns removed=true when the
// directory was removed immediately; rebootRequired is true only when every
// remaining entry was successfully scheduled for deletion.
func RemoveProgramData() (removed, rebootRequired bool, err error) {
	root := os.Getenv("ProgramData")
	if root == "" {
		return false, false, nil
	}
	dir := filepath.Join(root, "FaceLogin")
	if !DirExists(dir) {
		return false, false, nil
	}
	// Guard: base name must be exactly "FaceLogin". Belt-and-suspenders against
	// a tampered %ProgramData% pointing somewhere unexpected.
	if !strings.EqualFold(filepath.Base(filepath.Clean(dir)), "FaceLogin") {
		return false, false, fmt.Errorf("unsafe ProgramData cleanup path: %s", dir)
	}
	pending, removeErr := removeAllOrScheduleReboot(dir)
	if removeErr != nil {
		return false, pending, removeErr
	}
	return !pending, pending, nil
}
