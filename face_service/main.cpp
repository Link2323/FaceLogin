#include "FaceService.h"
#include "auth_worker.h"
#include "../common/logger.h"
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <limits>

// Named mutex to prevent multiple instances
static constexpr wchar_t SINGLE_INSTANCE_MUTEX[] =
    L"Global\\FaceLoginService_SingleInstance";

// Entry point for the Windows service executable.
// Usage:
//   FaceLoginService.exe                    — Run as service (SCM entry)
//   FaceLoginService.exe -install           — Install the service
//   FaceLoginService.exe -uninstall         — Uninstall the service
//   FaceLoginService.exe -standalone        — Run in foreground (for testing)
//   FaceLoginService.exe -auth-worker ...   — private parent-launched worker

namespace {

bool ParseInheritedHandle(const wchar_t* text, HANDLE& handle) {
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    const unsigned long long raw = std::wcstoull(text, &end, 10);
    if (!end || *end != L'\0' || raw == 0 ||
        raw > static_cast<unsigned long long>(std::numeric_limits<uintptr_t>::max())) {
        return false;
    }
    handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

bool TryRunAuthenticationWorker(int argc, wchar_t* argv[], int& exitCode) {
    int workerModeCount = 0;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"-auth-worker") == 0) ++workerModeCount;
    }
    if (workerModeCount == 0) return false;
    if (workerModeCount != 1 || argc != 6) {
        exitCode = ERROR_INVALID_PARAMETER;
        return true;
    }

    HANDLE parentToWorker = INVALID_HANDLE_VALUE;
    HANDLE workerToParent = INVALID_HANDLE_VALUE;
    bool haveInput = false;
    bool haveOutput = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"-auth-worker") == 0) {
            continue;
        } else if (_wcsicmp(argv[i], L"--in") == 0 && i + 1 < argc) {
            if (haveInput) {
                exitCode = ERROR_INVALID_PARAMETER;
                return true;
            }
            if (!ParseInheritedHandle(argv[++i], parentToWorker)) {
                exitCode = ERROR_INVALID_HANDLE;
                return true;
            }
            haveInput = true;
        } else if (_wcsicmp(argv[i], L"--out") == 0 && i + 1 < argc) {
            if (haveOutput) {
                exitCode = ERROR_INVALID_PARAMETER;
                return true;
            }
            if (!ParseInheritedHandle(argv[++i], workerToParent)) {
                exitCode = ERROR_INVALID_HANDLE;
                return true;
            }
            haveOutput = true;
        } else {
            exitCode = ERROR_INVALID_PARAMETER;
            return true;
        }
    }
    if (!haveInput || !haveOutput ||
        parentToWorker == INVALID_HANDLE_VALUE || workerToParent == INVALID_HANDLE_VALUE) {
        exitCode = ERROR_INVALID_HANDLE;
        return true;
    }
    exitCode = facelogin::RunAuthenticationWorker(parentToWorker, workerToParent);
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // The short-lived auth worker shares this executable but must not acquire
    // the service singleton: its parent already owns it. Worker mode verifies
    // LocalSystem and the inherited private handles in RunAuthenticationWorker.
    int workerExitCode = 0;
    if (TryRunAuthenticationWorker(argc, argv, workerExitCode)) {
        return workerExitCode;
    }

    // Prevent multiple instances
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, SINGLE_INSTANCE_MUTEX);
    if (hMutex == nullptr) {
        wprintf(L"ERROR: Failed to create singleton mutex (error %lu).\n",
                GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        wprintf(L"FaceLoginService is already running.\n");
        return 0;
    }
    // Mutex held for the lifetime of this process — released on exit.

    // Parse command line
    bool installMode = false;
    bool uninstallMode = false;
    bool standaloneMode = false;

    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"-install") == 0 || _wcsicmp(argv[i], L"/install") == 0) {
            installMode = true;
        }
        else if (_wcsicmp(argv[i], L"-uninstall") == 0 || _wcsicmp(argv[i], L"/uninstall") == 0) {
            uninstallMode = true;
        }
        else if (_wcsicmp(argv[i], L"-standalone") == 0 || _wcsicmp(argv[i], L"/standalone") == 0) {
            standaloneMode = true;
        }
    }

    // Handle install/uninstall
    if (installMode) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (facelogin::FaceService::Install(exePath)) {
            wprintf(L"FaceLoginService installed successfully.\n");
            wprintf(L"Start with: sc start FaceLoginService\n");
            return 0;
        }
        wprintf(L"ERROR: Failed to install FaceLoginService.\n");
        return 1;
    }

    if (uninstallMode) {
        if (facelogin::FaceService::Uninstall()) {
            wprintf(L"FaceLoginService uninstalled successfully.\n");
            return 0;
        }
        wprintf(L"ERROR: Failed to uninstall FaceLoginService.\n");
        return 1;
    }

    // Standalone mode (foreground, for testing)
    if (standaloneMode) {
        wprintf(L"Running FaceLoginService in standalone (foreground) mode...\n");
        wprintf(L"Press Ctrl+C to stop.\n\n");

        facelogin::Logger::Instance().SetEnableDebugOutput(true);
        facelogin::Logger::Instance().SetMinLevel(facelogin::LogLevel::Debug);

        // The service entry point starts its own loop
        facelogin::FaceService::RunStandalone();
        return 0;
    }

    // Default: run as a Windows service via SCM
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(L"FaceLoginService"),
          facelogin::FaceService::ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // Not started by SCM — print usage
            wprintf(L"FaceLoginService — Custom Face Recognition Login for Windows\n\n");
            wprintf(L"Usage:\n");
            wprintf(L"  FaceLoginService.exe                 Run as Windows service\n");
            wprintf(L"  FaceLoginService.exe -install        Install the service\n");
            wprintf(L"  FaceLoginService.exe -uninstall      Uninstall the service\n");
            wprintf(L"  FaceLoginService.exe -standalone     Run in foreground (testing)\n");
            return 0;
        }
        return static_cast<int>(err);
    }

    return 0;
}
