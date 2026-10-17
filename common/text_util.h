#pragma once

// Single implementation of the UTF-8 <-> wide-string conversions shared by
// the service (config camera device), the enrollment UI and the WebView
// host-object plumbing. Call sites with special needs (embedded NULs,
// fixed-size buffers) still call MultiByteToWideChar/WideCharToMultiByte
// directly.

#include <windows.h>
#include <string>

namespace facelogin {

// NUL-terminated UTF-8 -> wide. Empty in, empty out; conversion failure
// (invalid UTF-8) yields an empty string rather than partial text.
inline std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

// Wide -> NUL-terminated UTF-8, for JS/JSON payloads and log bridging.
inline std::string WstrToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

} // namespace facelogin
