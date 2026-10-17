#include "log_viewer.h"
#include "../common/logger.h"
#include "../common/text_util.h"

#include <cstring>
#include <sstream>
#include <vector>

namespace facelogin {

static const wchar_t* LogFileName(EnrollmentLogSource source) {
    switch (source) {
    case EnrollmentLogSource::Console: return L"console.log";
    case EnrollmentLogSource::Service: return L"service.log";
    case EnrollmentLogSource::AuthWorker: return L"auth_worker.log";
    case EnrollmentLogSource::CredentialProvider: return L"credential_provider.log";
    }
    return nullptr;
}

// Collapse <dataDir>\log\<fileName> to zero length. Safe against the live
// writers because the common Logger opens its file with FILE_APPEND_DATA
// (the kernel writes every record at the file's CURRENT end) and shares
// read/write, so collapsing the file to zero length just moves the append
// point — the next log line lands at offset 0, no holes, no corruption.
// Rotated <stem>.YYYY-MM-DD.log files are never shown in the viewer and are
// left untouched. A missing file counts as success (nothing to clear).
static bool TruncateLogFile(const std::wstring& dataDir, const std::wstring& fileName) {
    std::wstring logPath = dataDir + L"\\log\\" + fileName;
    HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
    bool ok = SetEndOfFile(hFile) != 0;
    CloseHandle(hFile);
    return ok;
}

static std::string ReadLogFile(const std::wstring& dataDir, const std::wstring& logFileName) {
    // Read the log file directly — avoids pipe message size limits.
    // Log files are UTF-8 without BOM; files from pre-UTF-8 builds were
    // UTF-16LE and are detected (BOM or 0x00 bytes — impossible in UTF-8)
    // for a graceful fallback while such files still exist.
    std::wstring logPath = dataDir + L"\\log\\" + logFileName;
    HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return "[\"" + WstrToUtf8(logFileName) + " not available\"]";
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize < 2) {
        CloseHandle(hFile);
        // Empty file (e.g. just cleared via ClearFileLog) renders as a blank
        // viewer, matching the Console source after ClearLog — no placeholder.
        return "[]";
    }

    // Cap at ~256KB of raw bytes, reading from the END — the viewer cares
    // about the most recent lines, and a day of logging can exceed the cap.
    DWORD capSize = fileSize;
    if (capSize > 256 * 1024) capSize = 256 * 1024;
    bool truncated = (capSize < fileSize);
    if (truncated &&
        SetFilePointer(hFile, static_cast<LONG>(fileSize - capSize),
                       nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return "[\"Failed to read " + WstrToUtf8(logFileName) + "\"]";
    }

    std::vector<char> buf(capSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf.data(), capSize, &bytesRead, nullptr) || bytesRead < 2) {
        CloseHandle(hFile);
        return "[\"Failed to read " + WstrToUtf8(logFileName) + "\"]";
    }
    CloseHandle(hFile);

    bool utf16 = (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE;
    if (!utf16) {
        for (DWORD i = 0; i < bytesRead; i++) {
            if (buf[i] == '\0') { utf16 = true; break; }
        }
    }

    std::string text;
    if (utf16) {
        // Legacy UTF-16LE file — decode to UTF-8, then share the byte-level
        // line parsing below.
        // A mid-file cut can land mid-UTF-16-character; legacy files are
        // near-pure ASCII (high byte 0x00), so a leading 0x00 before a
        // non-zero byte marks a one-byte misalignment — drop it before
        // pairing the bytes into wide chars.
        if (bytesRead >= 2 && (unsigned char)buf[0] == 0x00 && (unsigned char)buf[1] != 0x00) {
            buf.erase(buf.begin());
            bytesRead--;
        }
        int wlen = bytesRead / sizeof(wchar_t);
        std::wstring wide(wlen, L'\0');
        memcpy(&wide[0], buf.data(), wlen * sizeof(wchar_t));
        if (!wide.empty() && wide.front() == 0xFEFF)
            wide.erase(wide.begin());
        int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                       static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            text.resize(need);
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                static_cast<int>(wide.size()),
                                &text[0], need, nullptr, nullptr);
        }
    } else {
        text.assign(buf.data(), bytesRead);
    }

    // When the tail cut landed mid-line, the first line in `text` is a
    // partial line cut mid-write — drop it so the viewer never shows a
    // garbled fragment. (No newline at all means the whole cap window is one
    // fragment of a single huge line; show nothing rather than garbage.)
    if (truncated) {
        size_t nl = text.find('\n');
        if (nl == std::string::npos) text.clear();
        else text.erase(0, nl + 1);
    }

    // Parse lines: each log line ends with \r\n. Bytes pass through verbatim
    // (the file is UTF-8); only JSON metacharacters are escaped.
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    size_t pos = 0;
    while (pos < text.size()) {
        // Find end of line
        size_t lineStart = pos;
        while (pos < text.size() && text[pos] != '\r' && text[pos] != '\n') pos++;
        size_t lineLen = pos - lineStart;
        // Skip \r\n
        while (pos < text.size() && (text[pos] == '\r' || text[pos] == '\n')) pos++;
        if (lineLen == 0) continue;

        if (!first) ss << ",";
        first = false;
        ss << "\"";
        for (size_t i = 0; i < lineLen; i++) {
            char ch = text[lineStart + i];
            if (ch == '\\') ss << "\\\\";
            else if (ch == '"') ss << "\\\"";
            else ss << ch;
        }
        ss << "\"";
    }
    ss << "]";
    return ss.str();
}

std::string ReadEnrollmentLog(const std::wstring& dataDir, EnrollmentLogSource source) {
    const wchar_t* fileName = LogFileName(source);
    return fileName ? ReadLogFile(dataDir, fileName) : "[]";
}

bool ClearEnrollmentLog(const std::wstring& dataDir, EnrollmentLogSource source) {
    const wchar_t* fileName = LogFileName(source);
    if (!fileName) return false;
    if (source == EnrollmentLogSource::Console) Logger::Instance().ClearLogs();
    return TruncateLogFile(dataDir, fileName);
}

} // namespace facelogin
