#include "data_path.h"
#include "registry_util.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <vector>

namespace facelogin {

namespace {

// Returns the directory containing the current process EXE (no trailing
// backslash). Empty on failure.
std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"";
    std::wstring path(buf, len);
    // Strip the trailing component (the EXE filename).
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return path.substr(0, slash);
}

// Canonicalize a path: resolve to absolute, collapse "."/"..", strip a
// trailing backslash. Comparison via this form is case-insensitive on
// Windows but the canonical form preserves the input case. Returns empty
// on failure (e.g. drive letter missing).
std::wstring CanonicalizePath(const std::wstring& path) {
    if (path.empty()) return L"";
    wchar_t full[_MAX_PATH] = {};
    if (!_wfullpath(full, path.c_str(), _MAX_PATH)) return L"";
    std::wstring result(full);
    while (!result.empty() && result.back() == L'\\') result.pop_back();
    return result;
}

// Case-insensitive prefix check: does `path` start with `prefix\` (or equal
// `prefix`)? Used to test whether the EXE lives under Program Files.
bool StartsWithDir(const std::wstring& path, const std::wstring& prefix) {
    if (path.size() < prefix.size()) return false;
    if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
    // Either equal, or the next char in `path` must be a separator so we
    // don't match "C:\Program Filesx" against "C:\Program Files".
    return path.size() == prefix.size() || path[prefix.size()] == L'\\';
}

// True if the EXE runs from an ACL-protected install location (Program Files),
// i.e. production deployment. This is the trust-mode switch — it cannot be
// spoofed without admin + Windows Installer protection bypass.
bool IsProductionExePath(const std::wstring& exeDir) {
    return StartsWithDir(exeDir, L"C:\\Program Files\\") ||
           StartsWithDir(exeDir, L"C:\\Program Files (x86)\\");
}

// The default fallback directory (%ProgramData%\FaceLogin). Used only in
// development mode; production rejects it.
std::wstring GetProgramDataFallback() {
    wchar_t programData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        return CanonicalizePath(std::wstring(programData) + L"\\FaceLogin");
    }
    return L"C:\\ProgramData\\FaceLogin";
}

} // namespace

std::wstring ResolveSecureDataDir(const std::wstring& dataDirHint,
                                  std::wstring* outReason)
{
    const std::wstring exeDir = CanonicalizePath(GetExeDir());
    const bool production = IsProductionExePath(exeDir);

    // Build the candidate path with the same precedence as the legacy code:
    // caller hint > registry DataPath > [dev only] ProgramData fallback.
    std::wstring candidate;
    if (!dataDirHint.empty()) {
        candidate = dataDirHint;
    } else {
        std::wstring fromReg = ReadRegString(REGVAL_DATA_PATH, L"");
        if (!fromReg.empty()) {
            candidate = fromReg;
        } else if (!production) {
            candidate = GetProgramDataFallback();
        } else {
            // Production with no DataPath value is an anomaly (the installer
            // always writes it). Fall back to the EXE dir rather than fail —
            // the allow-list check below still validates it.
            candidate = exeDir;
        }
    }

    candidate = CanonicalizePath(candidate);
    if (candidate.empty()) {
        if (outReason) *outReason = L"path canonicalization failed";
        return L"";
    }

    // Build the trusted allow-list.
    std::vector<std::wstring> allowed;
    if (!exeDir.empty()) allowed.push_back(exeDir);
    if (!production) {
        allowed.push_back(GetProgramDataFallback());
        // A caller-supplied hint is trusted verbatim in dev mode.
        std::wstring canonHint = CanonicalizePath(dataDirHint);
        if (!canonHint.empty()) allowed.push_back(canonHint);
    }

    // Check membership (case-insensitive, canonical paths).
    for (const auto& a : allowed) {
        if (_wcsicmp(a.c_str(), candidate.c_str()) == 0) {
            return candidate;
        }
    }

    if (outReason) {
        *outReason = L"DataPath '" + candidate + L"' outside trusted allow-list (mode=" +
                     (production ? L"production" : L"development") + L")";
    }
    return L"";
}

} // namespace facelogin
