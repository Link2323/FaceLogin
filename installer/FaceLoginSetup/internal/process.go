package internal

import (
	"fmt"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

// KillProcessesByName terminates every running process whose image name
// matches imageName case-insensitively (e.g. "FaceLoginConsole.exe") and
// waits for each to exit, so the image lock the process holds on its exe is
// released before the caller overwrites or deletes that file. Returns how
// many processes were terminated. Processes that cannot be opened (access
// denied) are skipped silently.
func KillProcessesByName(imageName string) (int, error) {
	snapshot, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return 0, fmt.Errorf("enumerate processes: %w", err)
	}
	defer windows.CloseHandle(snapshot)

	killed := 0
	entry := windows.ProcessEntry32{Size: uint32(unsafe.Sizeof(windows.ProcessEntry32{}))}
	for err = windows.Process32First(snapshot, &entry); err == nil; err = windows.Process32Next(snapshot, &entry) {
		if !strings.EqualFold(windows.UTF16ToString(entry.ExeFile[:]), imageName) {
			continue
		}
		p, e := windows.OpenProcess(windows.PROCESS_TERMINATE|windows.SYNCHRONIZE, false, entry.ProcessID)
		if e != nil {
			continue
		}
		if windows.TerminateProcess(p, 1) == nil {
			killed++
			// TerminateProcess is asynchronous — the exe stays locked until
			// the process object is signalled. Bounded wait; a process that
			// somehow survives just falls back to the caller's error path.
			windows.WaitForSingleObject(p, 5000)
		}
		windows.CloseHandle(p)
	}
	return killed, nil
}
