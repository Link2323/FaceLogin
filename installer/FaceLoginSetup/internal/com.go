package internal

import (
	"fmt"
	"os"
	"os/exec"

	"golang.org/x/sys/windows"
)

// clsidCredentialProvider is the COM CLSID of FaceLoginCredentialProvider,
// used for manual registry cleanup when the DLL is no longer available.
const clsidCredentialProvider = "{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}"

// callSelfReg loads dllPath in-process and invokes DllRegisterServer
// (register == true) or DllUnregisterServer (register == false).
//
// The installer process runs at high integrity (after Elevate), so the DLL's
// self-registration can write the HKLM keys it needs. This replaces the old
// regsvr32.exe approach: regsvr32 has an asInvoker manifest and runs at medium
// integrity even when spawned by an elevated process, so its DllRegisterServer
// call was denied write access to HKLM (regsvr32 reported exit code 3). Calling
// the export in-process is exactly what regsvr32 does internally — we just skip
// the medium-integrity hop.
func callSelfReg(dllPath string, register bool) error {
	dll, err := windows.LoadDLL(dllPath)
	if err != nil {
		return fmt.Errorf("LoadLibrary %s: %w", dllPath, err)
	}
	defer dll.Release()

	procName := "DllRegisterServer"
	if !register {
		procName = "DllUnregisterServer"
	}
	proc, err := dll.FindProc(procName)
	if err != nil {
		return fmt.Errorf("GetProcAddress %s: %w", procName, err)
	}

	// STDAPI DllRegisterServer(void) / DllUnregisterServer(void) — no args.
	// HRESULT is a 32-bit signed value returned in r1; convert via int64 to
	// preserve the sign (FAILED(hr) is hr < 0, e.g. E_ACCESSDENIED=0x80070005).
	r1, _, _ := proc.Call()
	hr := int32(int64(r1))
	if hr < 0 {
		return fmt.Errorf("%s failed: HRESULT 0x%08X", procName, uint32(hr))
	}
	return nil
}

// RegisterCOMDLL registers the credential provider COM DLL by invoking
// DllRegisterServer in-process.
func RegisterCOMDLL(dllPath string) error {
	if !FileExists(dllPath) {
		return fmt.Errorf("DLL not found: %s", dllPath)
	}
	return callSelfReg(dllPath, true)
}

// UnregisterCOMDLL unregisters the credential provider COM DLL. If the DLL
// file is still present, it invokes DllUnregisterServer in-process; otherwise
// (e.g. partial uninstall where files are already gone) it removes the
// registry keys directly.
func UnregisterCOMDLL(dllPath string) error {
	if dllPath != "" && FileExists(dllPath) {
		return callSelfReg(dllPath, false)
	}
	cleanCredentialProviderRegistry()
	return nil
}

// cleanCredentialProviderRegistry removes the CLSID and Credential Provider
// registry entries directly, used when the DLL (and thus DllUnregisterServer)
// is no longer available.
func cleanCredentialProviderRegistry() {
	cpKey := fmt.Sprintf(
		`SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\%s`,
		clsidCredentialProvider,
	)
	exec.Command("reg", "delete", fmt.Sprintf(`HKLM\%s`, cpKey), "/f").Run()
	exec.Command("reg", "delete", fmt.Sprintf(`HKCR\CLSID\%s`, clsidCredentialProvider), "/f").Run()
}

// SetDirectoryACL replaces (rather than merely augments) the directory ACL
// with inheritable entries for SYSTEM, Administrators (Full Control) and
// Users (Read & Execute). The Users RX entry is required because the
// credential provider DLL is loaded by LogonUI.exe (lock screen) and by the
// installer's in-process DllRegisterServer call; those load paths need
// non-administrative read access to the file. Numeric SIDs avoid failures on
// non-English Windows installations.
func SetDirectoryACL(dirPath string) error {
	if !DirExists(dirPath) {
		if err := os.MkdirAll(dirPath, 0755); err != nil {
			return err
		}
	}

	// ACL strategy, verified to make the deployed DLL loadable while keeping
	// the directory tamper-resistant:
	//   1. reset/T       — strip any pre-existing explicit ACEs from dir + files
	//   2. grant:r/T      — write the three trusted principals as EXPLICIT ACEs
	//                       onto every file (these survive step 4 because they
	//                       are explicit, not inherited)
	//   3. setowner/T     — trusted owner
	//   4. inheritance:r  — protect ONLY the directory itself (no /T!). With /T
	//                       this would also strip inheritance from files, but on
	//                       files whose inherited ACEs were just reset/removed
	//                       that leaves them empty → LoadLibrary fails with
	//                       error 5. Without /T, the directory is protected
	//                       (parent ACL changes can't propagate in) while files
	//                       keep their explicit ACEs from step 2. New files
	//                       created later inherit (OI)(CI) entries from the dir.
	commands := [][]string{
		{dirPath, "/reset", "/T", "/Q"},
		{dirPath, "/grant:r", "*S-1-5-18:(OI)(CI)F", "*S-1-5-32-544:(OI)(CI)F", "*S-1-5-32-545:(OI)(CI)RX", "/T", "/Q"},
		{dirPath, "/setowner", "*S-1-5-32-544", "/T", "/Q"},
		{dirPath, "/inheritance:r", "/Q"},
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
