package internal

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/sys/windows"
)

// isElevated reports whether the test process runs with an elevated token.
// icacls /setowner Administrators on files the process owns needs it.
func isElevated() bool {
	var token windows.Token
	if err := windows.OpenProcessToken(windows.CurrentProcess(), windows.TOKEN_QUERY, &token); err != nil {
		return false
	}
	defer token.Close()
	return token.IsElevated()
}

// sddlFor returns the SDDL string of a file-system object's DACL (and owner).
func sddlFor(t *testing.T, path string) string {
	t.Helper()
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		t.Fatalf("GetNamedSecurityInfo(%s): %v", path, err)
	}
	return sd.String()
}

// TestSetDataDirectoryACLLocksOutUsers verifies the security property the
// function exists for, using the exact installer sequence: after the
// install-dir sweep (which grants Users RX everywhere), the data-specific
// lockdown must leave NO Users ACE on data\ or its files while
// SYSTEM/Administrators keep full access — and the install dir itself must
// KEEP its Users RX entry (the credential provider DLL load path needs it).
func TestSetDataDirectoryACLLocksOutUsers(t *testing.T) {
	if !isElevated() {
		t.Skip("icacls /setowner Administrators requires an elevated token")
	}

	base := t.TempDir()
	installDir := filepath.Join(base, "FaceLogin")
	dataDir := filepath.Join(installDir, "data")
	usersDat := filepath.Join(dataDir, "users.dat")
	if err := os.MkdirAll(dataDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(usersDat, []byte("x"), 0644); err != nil {
		t.Fatal(err)
	}

	if err := SetDirectoryACL(installDir); err != nil {
		t.Fatalf("SetDirectoryACL: %v", err)
	}
	if err := SetDataDirectoryACL(dataDir); err != nil {
		t.Fatalf("SetDataDirectoryACL: %v", err)
	}

	for _, path := range []string{dataDir, usersDat} {
		sddl := sddlFor(t, path)
		if strings.Contains(sddl, ";;;BU") || strings.Contains(sddl, "S-1-5-32-545") {
			t.Errorf("%s still grants Users access: %s", path, sddl)
		}
		if !strings.Contains(sddl, ";;;SY") || !strings.Contains(sddl, ";;;BA") {
			t.Errorf("%s lost SYSTEM/Administrators access: %s", path, sddl)
		}
	}

	// The install dir must remain Users-readable (DLL loaded by LogonUI).
	if sddl := sddlFor(t, installDir); !strings.Contains(sddl, ";;;BU") {
		t.Errorf("install dir lost its Users RX entry: %s", sddl)
	}

	// A file created AFTER the lockdown (first real enrollment) must inherit
	// only SYSTEM/Administrators from the protected data dir.
	lateFile := filepath.Join(dataDir, "config.json")
	if err := os.WriteFile(lateFile, []byte("{}"), 0644); err != nil {
		t.Fatal(err)
	}
	if sddl := sddlFor(t, lateFile); strings.Contains(sddl, ";;;BU") || strings.Contains(sddl, "S-1-5-32-545") {
		t.Errorf("file created after lockdown inherits Users access: %s", sddl)
	}
}
