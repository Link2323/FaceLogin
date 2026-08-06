package internal

import (
	"crypto/sha256"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
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
		size:         16272909,
		sha256:       "c940f97765fdc4b872b4a1ea041248d3e3d550202b7639f9488be558a6c0acb0",
	},
	{
		embeddedPath: "resources/models/w600k_r50.onnx",
		fileName:     "w600k_r50.onnx",
		size:         174383860,
		sha256:       "4c06341c33c2ca1f86781dab0e829f88ad5b64be9fba56e56bc9ebdefc619e43",
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

// Files deployed by earlier releases but intentionally absent from the current
// manifest. Upgrade and uninstall remove only these exact legacy names.
var legacyModelFiles = []string{
	"OULU_Protocol_2_model_0_0.onnx",
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

// RemoveInstalledFiles deletes exactly the files this installer deployed, in
// the same layout ExtractAll wrote them. It NEVER removes the install directory
// or any user data (data/, log/). Uninstall uses this instead of os.RemoveAll
// so a corrupted/malicious InstallPath registry value can never wipe an
// arbitrary directory. Returns the number of files removed and the first error
// (if any) — callers can continue and report a summary.
//
// removeUserData: when true, additionally deletes the data/ (config.json,
// enrolled face database) and log/ (logs) subdirectories — i.e. a full purge.
// When false, only program files are removed and user data is preserved.
func RemoveInstalledFiles(destDir string, removeUserData bool) (int, error) {
	removed := 0
	var firstErr error
	recordErr := func(err error) {
		if err != nil && firstErr == nil {
			firstErr = err
		}
	}

	modelsDir := filepath.Join(destDir, "models")

	entries, err := fs.ReadDir(EmbeddedFS, "resources")
	if err != nil {
		// If embedded resources can't be enumerated, fall back to the known
		// top-level binary names so uninstall still removes the executables.
		for _, name := range []string{
			"FaceLoginService.exe",
			"FaceLoginCredentialProvider.dll",
			"FaceLoginConsole.exe",
		} {
			p := filepath.Join(destDir, name)
			if FileExists(p) {
				if err := os.Remove(p); err != nil {
					recordErr(err)
				} else {
					removed++
				}
			}
		}
		for _, name := range legacyModelFiles {
			p := filepath.Join(modelsDir, name)
			if FileExists(p) {
				if err := os.Remove(p); err != nil {
					recordErr(err)
				} else {
					removed++
				}
			}
		}
		return removed, firstErr
	}
	modelEntries, _ := fs.ReadDir(EmbeddedFS, "resources/models")

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		var dstPath string
		if filepath.Ext(name) == ".dat" {
			dstPath = filepath.Join(modelsDir, name)
		} else {
			dstPath = filepath.Join(destDir, name)
		}
		if FileExists(dstPath) {
			if err := os.Remove(dstPath); err != nil {
				recordErr(err)
			} else {
				removed++
			}
		}
	}
	for _, entry := range modelEntries {
		if entry.IsDir() {
			continue
		}
		dstPath := filepath.Join(modelsDir, entry.Name())
		if FileExists(dstPath) {
			if err := os.Remove(dstPath); err != nil {
				recordErr(err)
			} else {
				removed++
			}
		}
	}
	for _, name := range legacyModelFiles {
		dstPath := filepath.Join(modelsDir, name)
		if FileExists(dstPath) {
			if err := os.Remove(dstPath); err != nil {
				recordErr(err)
			} else {
				removed++
			}
		}
	}

	// Full purge: also remove user data and logs (config.json, the enrolled
	// face database in data/, and log/). Only invoked when the caller opted in
	// (removeUserData); the default uninstall preserves these.
	if removeUserData {
		for _, sub := range []string{"data", "log"} {
			dir := filepath.Join(destDir, sub)
			if DirExists(dir) {
				if err := os.RemoveAll(dir); err != nil {
					recordErr(err)
				} else {
					removed++
				}
			}
		}
	}

	return removed, firstErr
}

// RemoveInstalledDir removes the install directory itself, but ONLY if it is
// empty after the files above were deleted. This is the anti-misdeletion guard:
// a real FaceLogin install dir that held unexpected/unknown files (not deployed
// by us) will still contain them here, so the directory is left in place and
// false is returned — never silently wiping an arbitrary directory. Returns
// true when the directory was removed.
func RemoveInstalledDir(destDir string) (bool, error) {
	if !DirExists(destDir) {
		return true, nil // already gone
	}
	entries, err := os.ReadDir(destDir)
	if err != nil {
		return false, err
	}
	// Also tolerate the models/ subdir being left empty (it's ours) — but only
	// if it contains nothing. Anything else means the dir is NOT empty.
	if len(entries) == 0 {
		if err := os.Remove(destDir); err != nil {
			return false, err
		}
		return true, nil
	}
	// If only an empty models/ remains, remove it then retry.
	onlyModels := len(entries) == 1 && entries[0].IsDir() && strings.EqualFold(entries[0].Name(), "models")
	if onlyModels {
		mEntries, _ := os.ReadDir(filepath.Join(destDir, "models"))
		if len(mEntries) == 0 {
			if err := os.Remove(filepath.Join(destDir, "models")); err != nil {
				return false, err
			}
			if err := os.Remove(destDir); err != nil {
				return false, err
			}
			return true, nil
		}
	}
	return false, nil // not empty — do NOT delete
}
