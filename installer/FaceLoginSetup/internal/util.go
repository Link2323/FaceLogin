package internal

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

// ReadRegString reads a REG_SZ from HKLM\SOFTWARE\FaceLogin.
// Returns defaultValue if missing.
func ReadRegString(valueName, defaultValue string) string {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`, registry.QUERY_VALUE)
	if err != nil {
		return defaultValue
	}
	defer k.Close()

	val, _, err := k.GetStringValue(valueName)
	if err != nil {
		return defaultValue
	}
	return val
}

// WriteRegString writes a REG_SZ to HKLM\SOFTWARE\FaceLogin (creates key if needed).
func WriteRegString(valueName, value string) error {
	k, _, err := registry.CreateKey(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`, registry.SET_VALUE)
	if err != nil {
		return fmt.Errorf("create/open registry key: %w", err)
	}
	defer k.Close()
	return k.SetStringValue(valueName, value)
}

// DeleteRegValue deletes a value from HKLM\SOFTWARE\FaceLogin.
func DeleteRegValue(valueName string) error {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`, registry.SET_VALUE)
	if err != nil {
		return nil // key already gone
	}
	defer k.Close()
	return k.DeleteValue(valueName)
}

// deleteRegKeyTree recursively removes a registry key's subkeys and values.
// RegDeleteKey cannot remove a key that still has subkeys, and runtime
// components may create children below the FaceLogin key, so uninstall must
// delete depth-first.
func deleteRegKeyTree(parent registry.Key, subpath string) error {
	k, err := registry.OpenKey(parent, subpath,
		registry.ENUMERATE_SUB_KEYS|registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		if err == registry.ErrNotExist {
			return nil
		}
		return err
	}

	subs, err := k.ReadSubKeyNames(-1)
	if err != nil {
		k.Close()
		return err
	}
	for _, sub := range subs {
		if err := deleteRegKeyTree(k, sub); err != nil {
			k.Close()
			return err
		}
	}

	values, err := k.ReadValueNames(-1)
	if err != nil {
		k.Close()
		return err
	}
	for _, value := range values {
		if err := k.DeleteValue(value); err != nil && err != registry.ErrNotExist {
			k.Close()
			return err
		}
	}

	// Close before deleting the key itself so no child handle remains open.
	if err := k.Close(); err != nil {
		return err
	}
	return registry.DeleteKey(parent, subpath)
}

// DeleteRegKey removes the entire HKLM\SOFTWARE\FaceLogin key tree. Use on
// uninstall: the service writes runtime values (e.g. UserLoggedIn) that the
// installer never created, and future versions may write subkeys as well.
func DeleteRegKey() error {
	err := deleteRegKeyTree(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`)
	if err != nil && err != registry.ErrNotExist {
		return fmt.Errorf("delete registry key tree SOFTWARE\\FaceLogin: %w", err)
	}
	return nil
}

// Path helpers

// GetDefaultInstallDir returns the default install path.
func GetDefaultInstallDir() string {
	return filepath.Join(os.Getenv("ProgramFiles"), "FaceLogin")
}

// FileExists checks if a file exists and is not a directory.
func FileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

// DirExists checks if a directory exists.
func DirExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

// IsSafeInstallDir returns true only when path looks like a real FaceLogin
// install directory, i.e. the directory name contains "FaceLogin" (case-
// insensitive) and it holds the service executable or another known install
// marker. Used before an
// uninstall deletes the tree: without this guard, a corrupted/malicious
// InstallPath registry value (e.g. "C:\" or "C:\Users\<user>") would make
// os.RemoveAll recursively delete an arbitrary directory — catastrophic data
// loss. When this returns false, the uninstall must NOT delete the directory.
func IsSafeInstallDir(path string) bool {
	if !DirExists(path) {
		return false
	}

	// 1) Directory name must contain "FaceLogin" (case-insensitive).
	base := strings.ToLower(filepath.Base(filepath.Clean(path)))
	if !strings.Contains(base, "facelogin") {
		return false
	}

	// 2) A current install contains the service executable. An older uninstall
	//    may already have removed that file while leaving a known payload or
	//    runtime-data directory behind, so accept those explicit markers too.
	svcPath := filepath.Join(path, "FaceLoginService.exe")
	if FileExists(svcPath) {
		return true
	}
	for _, marker := range []string{
		"FaceLoginConsole.exe",
		"FaceLoginCredentialProvider.dll",
		"PadCalibration.exe",
		"EmbeddingTest.exe",
		"CameraLifecycleTest.exe",
		"models",
		"data",
		"log",
	} {
		markerPath := filepath.Join(path, marker)
		if FileExists(markerPath) || DirExists(markerPath) {
			return true
		}
	}
	return false
}

// CopyFile copies a file from src to dst. Parent directories of dst must exist.
func CopyFile(src, dst string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return fmt.Errorf("read %s: %w", src, err)
	}
	return os.WriteFile(dst, data, 0644)
}

// RunCommand runs a console command and returns stdout+stderr combined.
// The installer is a GUI-subsystem process, so every console tool it spawns
// (icacls, taskkill, reg) would otherwise flash its own console window;
// CREATE_NO_WINDOW suppresses that window. All external command launches
// must go through this helper — a bare exec.Command elsewhere will flash.
func RunCommand(name string, args ...string) (string, error) {
	cmd := exec.Command(name, args...)
	cmd.SysProcAttr = &syscall.SysProcAttr{
		HideWindow:    true,
		CreationFlags: windows.CREATE_NO_WINDOW,
	}
	out, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

// StartProgram launches a GUI program detached: Start() returns immediately,
// the child outlives the installer. The child inherits the installer's
// elevated token, so the enrollment console opens without its own UAC prompt.
//
// Wails's go-webview2 leaves the WEBVIEW2_* env vars set to EMPTY strings in
// this process (preventEnvAndRegistryOverrides). The official WebView2 loader
// checks existence, not emptiness: an inherited empty WEBVIEW2_USER_DATA_FOLDER
// silently overrides the app's own userDataFolder argument and makes the
// console fall back to the exe-adjacent default — ACL-locked under Program
// Files, so WebView2 dies there. Strip every variable go-webview2 blanks.
func StartProgram(path string) error {
	cmd := exec.Command(path)
	cmd.Dir = filepath.Dir(path)
	clean := make([]string, 0, len(os.Environ()))
	for _, kv := range os.Environ() {
		name := kv
		if i := strings.IndexByte(kv, '='); i >= 0 {
			name = kv[:i]
		}
		switch strings.ToUpper(name) {
		case "WEBVIEW2_USER_DATA_FOLDER", "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",
			"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "WEBVIEW2_RELEASE_CHANNEL_PREFERENCE",
			"WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER":
			continue
		}
		clean = append(clean, kv)
	}
	cmd.Env = clean
	return cmd.Start()
}
