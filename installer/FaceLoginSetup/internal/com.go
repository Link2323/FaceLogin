package internal

import (
	"fmt"
	"os"
	"os/exec"
)

// RegisterCOMDLL registers a COM DLL via regsvr32.
func RegisterCOMDLL(dllPath string) error {
	if !FileExists(dllPath) {
		return fmt.Errorf("DLL not found: %s", dllPath)
	}
	cmd := exec.Command("regsvr32", "/s", dllPath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("regsvr32 failed: %w\n%s", err, string(out))
	}
	return nil
}

// UnregisterCOMDLL unregisters a COM DLL via regsvr32.
// If the DLL file is already gone, cleans registry directly.
func UnregisterCOMDLL(dllPath string) error {
	if FileExists(dllPath) {
		cmd := exec.Command("regsvr32", "/s", "/u", dllPath)
		out, err := cmd.CombinedOutput()
		if err != nil {
			return fmt.Errorf("regsvr32 /u failed: %w\n%s", err, string(out))
		}
		return nil
	}

	// DLL gone — clean registry keys directly
	clsid := "{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}"
	cpKey := fmt.Sprintf(
		`SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\%s`,
		clsid,
	)
	exec.Command("reg", "delete", fmt.Sprintf(`HKLM\%s`, cpKey), "/f").Run()
	exec.Command("reg", "delete", fmt.Sprintf(`HKCR\CLSID\%s`, clsid), "/f").Run()
	return nil
}

// SetDirectoryACL replaces (rather than merely augments) the directory ACL
// with inheritable Full Control entries for SYSTEM and Administrators only.
// Numeric SIDs avoid failures on non-English Windows installations.
func SetDirectoryACL(dirPath string) error {
	if !DirExists(dirPath) {
		if err := os.MkdirAll(dirPath, 0755); err != nil {
			return err
		}
	}

	// /reset first removes any pre-existing explicit ACEs. We then add the
	// two trusted principals while inherited access is still available, set a
	// trusted owner, and finally remove every inherited ACE. New files inherit
	// only these two entries from the protected root directory.
	commands := [][]string{
		{dirPath, "/reset", "/T", "/Q"},
		{dirPath, "/grant:r", "*S-1-5-18:(OI)(CI)F", "*S-1-5-32-544:(OI)(CI)F", "/T", "/Q"},
		{dirPath, "/setowner", "*S-1-5-32-544", "/T", "/Q"},
		{dirPath, "/inheritance:r", "/T", "/Q"},
	}
	for _, args := range commands {
		cmd := exec.Command("icacls", args...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			return fmt.Errorf("icacls %v failed: %w\n%s", args[1:], err, string(out))
		}
	}
	return nil
}
