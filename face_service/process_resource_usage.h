#pragma once

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "psapi.lib")

namespace facelogin {

struct ProcessResourceUsage {
    DWORD handles = 0;
    DWORD threads = 0;
    double privateMiB = 0.0;
    double workingSetMiB = 0.0;
};

inline ProcessResourceUsage CurrentProcessResourceUsage() {
    ProcessResourceUsage usage;
    GetProcessHandleCount(GetCurrentProcess(), &usage.handles);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
                    ++usage.threads;
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        constexpr double kMiB = 1024.0 * 1024.0;
        usage.privateMiB = static_cast<double>(counters.PrivateUsage) / kMiB;
        usage.workingSetMiB = static_cast<double>(counters.WorkingSetSize) / kMiB;
    }
    return usage;
}

} // namespace facelogin
