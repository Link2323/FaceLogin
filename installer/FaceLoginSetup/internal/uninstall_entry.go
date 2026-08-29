package internal

import (
	"fmt"
	"path/filepath"

	"golang.org/x/sys/windows/registry"
)

// uninstallRegKey is where Windows lists apps in Settings → Apps and the
// classic appwiz.cpl. An entry here is the only standard way users can find
// the uninstaller after deleting the setup exe.
const uninstallRegKey = `SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\FaceLogin`

// WriteUninstallEntry registers FaceLogin in Add/Remove Programs.
// UninstallString points at the slim uninstaller deployed by the installer
// into the install directory (uninstall.exe is always in uninstall mode, so
// no arguments are needed). NoModify/NoRepair hide the change/repair buttons
// — the installer has no separate repair flow, reinstalling covers it.
func WriteUninstallEntry(installDir, version string) error {
	uninstaller := filepath.Join(installDir, "uninstall.exe")
	k, _, err := registry.CreateKey(registry.LOCAL_MACHINE, uninstallRegKey, registry.SET_VALUE)
	if err != nil {
		return fmt.Errorf("create uninstall entry key: %w", err)
	}
	defer k.Close()

	set := func(name, value string) error {
		if err := k.SetStringValue(name, value); err != nil {
			return fmt.Errorf("set %s: %w", name, err)
		}
		return nil
	}
	if err := set("DisplayName", "FaceLogin 人脸识别登录"); err != nil {
		return err
	}
	if err := set("DisplayVersion", version); err != nil {
		return err
	}
	if err := set("Publisher", "FaceLogin"); err != nil {
		return err
	}
	// Quoted: the path usually contains spaces (C:\Program Files\...).
	if err := set("UninstallString", `"`+uninstaller+`"`); err != nil {
		return err
	}
	if err := set("DisplayIcon", filepath.Join(installDir, "FaceLoginConsole.exe")); err != nil {
		return err
	}
	if err := k.SetDWordValue("NoModify", 1); err != nil {
		return fmt.Errorf("set NoModify: %w", err)
	}
	if err := k.SetDWordValue("NoRepair", 1); err != nil {
		return fmt.Errorf("set NoRepair: %w", err)
	}
	return nil
}

// DeleteUninstallEntry removes the Add/Remove Programs registration.
func DeleteUninstallEntry() error {
	err := registry.DeleteKey(registry.LOCAL_MACHINE, uninstallRegKey)
	if err != nil && err != registry.ErrNotExist {
		return fmt.Errorf("delete uninstall entry: %w", err)
	}
	return nil
}
