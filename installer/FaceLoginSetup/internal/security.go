package internal

import (
	"fmt"

	"golang.org/x/sys/windows"
)

// regKeyObjectName is the SetNamedSecurityInfo object-name form for
// HKLM\SOFTWARE\FaceLogin. Using the name form (rather than a registry.Key
// handle opened with WRITE_DAC) sidesteps the fact that
// golang.org/x/sys/windows/registry exposes no WRITE_DAC access constant.
const regKeyObjectName = `MACHINE\SOFTWARE\FaceLogin`

// SetRegistryKeyACL applies a restrictive, protected DACL to
// HKLM\SOFTWARE\FaceLogin so that only SYSTEM and Administrators can change
// the DataPath/InstallPath values (which the service trusts for its data
// directory). Users retain read access (the lock-screen credential provider,
// hosted by LogonUI, must read these values to locate users.dat).
//
// This is the registry-side half of security #3 (DataPath redirection
// defense); the C++ service-side allow-list check is the other half.
//
// SDDL breakdown:
//   D:                        DACL follows
//   P                         protected — do NOT inherit HKLM\SOFTWARE's ACL
//   (A;OICI;KA;;;SY)          SYSTEM: key all access (service runs as SYSTEM)
//   (A;OICI;KA;;;BA)          Administrators: key all access (installer, admin tools)
//   (A;OICI;KR;;;BU)          Built-in Users: key read (LogonUI/credential provider)
func SetRegistryKeyACL() error {
	const sddl = `D:P(A;OICI;KA;;;SY)(A;OICI;KA;;;BA)(A;OICI;KR;;;BU)`

	sd, err := windows.SecurityDescriptorFromString(sddl)
	if err != nil {
		return fmt.Errorf("parse SDDL: %w", err)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("extract DACL from security descriptor: %w", err)
	}
	if dacl == nil {
		return fmt.Errorf("security descriptor has no DACL")
	}

	const info = windows.DACL_SECURITY_INFORMATION |
		windows.PROTECTED_DACL_SECURITY_INFORMATION
	if err := windows.SetNamedSecurityInfo(
		regKeyObjectName,
		windows.SE_REGISTRY_KEY,
		info,
		nil, // owner — unchanged
		nil, // group — unchanged
		dacl,
		nil, // sacl — unchanged
	); err != nil {
		return fmt.Errorf("set registry key DACL: %w", err)
	}
	return nil
}
