#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include "WebviewHost.h"
#include "EnrollmentWizard.h"
#include "../common/logger.h"
#include "../common/registry_util.h"

// ============================================================================
// Check admin elevation; re-launch if needed
// ============================================================================
static bool EnsureAdmin() {
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size))
            isElevated = elevation.TokenIsElevated;
        CloseHandle(hToken);
    }
    if (!isElevated) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = exePath;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) return true;
        MessageBoxW(nullptr,
            L"This application requires Administrator privileges to set up face login.",
            L"Administrator Required", MB_ICONERROR);
        return true;
    }
    return false; // already elevated, continue
}

// ============================================================================
// WinMain
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     LPWSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (EnsureAdmin()) return 0;

    // Single instance: a second console would fight over the camera and show
    // its own device picker. The mutex dies with the process, so a crash can
    // never leave a stale lock behind. If mutex creation itself fails, run
    // unprotected rather than blocking the app.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE,
                                        L"Local\\FaceLoginConsoleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"FaceLogin 控制台已在运行。",
                    L"FaceLogin Console", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        CloseHandle(instanceMutex);
        return 0;
    }

    // Check models exist
    std::wstring modelsDir;
    {
        std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
        if (!regData.empty())
            modelsDir = regData + L"\\models";
        else {
            wchar_t programData[MAX_PATH];
            SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData);
            modelsDir = std::wstring(programData) + L"\\FaceLogin\\models";
        }
    }
    // Check models exist. The enrollment app uses the ONNX pipeline
    // exclusively (SCRFD detection + InsightFace recognition); no landmark
    // model is loaded at runtime anymore (the 5 keypoints come from SCRFD).
    std::wstring detPath = modelsDir + L"\\det_10g_gnkps.onnx";
    std::wstring recPath = modelsDir + L"\\w600k_r50.onnx";

    if (GetFileAttributesW(detPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(recPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring msg = L"Face recognition models are missing.\n\n"
            L"Expected files:\n  " + detPath + L"\n  " + recPath;
        MessageBoxW(nullptr, msg.c_str(), L"Models Not Found", MB_ICONERROR);
        return 1;
    }

    // COM for WebView2 + WIC
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    facelogin::EnrollmentWizard wizard;

    // Repair records enrolled while GetUserNameExW(NameUserPrincipal) failed
    // (err 1332 on some machines): backfill the MSA UPN into the current
    // session user's record if it is still empty. Best-effort, never fatal —
    // must run after the wizard constructor (which resolves m_sid) and before
    // the UI loop so the first GetAccountType() the page reads is correct.
    wizard.AutoRepairEmptyUpnOnStartup();

    WebviewHost host(hInstance, &wizard);

    int result = host.Run();

    CoUninitialize();
    return result;
}
