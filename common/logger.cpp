#include "logger.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace facelogin {

Logger& Logger::Instance() {
    static Logger s_instance;
    if (!s_instance.m_csInitialized) {
        InitializeCriticalSection(&s_instance.m_cs);
        s_instance.m_csInitialized = true;
    }
    if (!s_instance.m_ringCsInitialized) {
        InitializeCriticalSection(&s_instance.m_ringCs);
        s_instance.m_ringBuffer.resize(RING_SIZE);
        s_instance.m_ringCsInitialized = true;
    }
    return s_instance;
}

void Logger::SetLogFile(const std::wstring& path) {
    EnterCriticalSection(&m_cs);
    // Ensure parent directory exists
    {
        std::wstring dir = path;
        size_t pos = dir.rfind(L'\\');
        if (pos != std::wstring::npos) {
            dir = dir.substr(0, pos);
            CreateDirectoryW(dir.c_str(), nullptr);
        }
    }
    m_logPath = path;
    // If an existing log already exceeds the cap (e.g. from a run before
    // rotation existed), rotate it now. CheckRotation works on the path,
    // independent of m_hFile, so it is safe to call before opening.
    CheckRotation();
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }
    OpenLogFile();
    LeaveCriticalSection(&m_cs);
}

void Logger::SetMinLevel(LogLevel level) {
    m_minLevel = level;
}

void Logger::Log(LogLevel level, const wchar_t* format, ...) {
    if (level < m_minLevel) return;

    va_list args;
    va_start(args, format);

    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);

    // Timestamp prefix
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t finalMsg[2560];
    const wchar_t* levelStr = L"";
    switch (level) {
        case LogLevel::Debug:   levelStr = L"DEBUG"; break;
        case LogLevel::Info:    levelStr = L"INFO"; break;
        case LogLevel::Warning: levelStr = L"WARN"; break;
        case LogLevel::Error:   levelStr = L"ERROR"; break;
    }

    _snwprintf_s(finalMsg, _TRUNCATE,
                  L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] [%lu] %s\r\n",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  levelStr, GetCurrentThreadId(), buffer);

    // Output to debugger
    if (m_debugOutput) {
        OutputDebugStringW(finalMsg);
    }

    // Write to file
    WriteToFile(finalMsg);

    // Ring buffer for UI — store WITHOUT trailing \r\n for cleaner display
    {
        std::wstring clean = finalMsg;
        while (!clean.empty() && (clean.back() == L'\r' || clean.back() == L'\n'))
            clean.pop_back();
        AppendToRingBuffer(clean);
    }
}

void Logger::Debug(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Debug, L"%s", buffer);
}

void Logger::Info(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Info, L"%s", buffer);
}

void Logger::Warning(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Warning, L"%s", buffer);
}

void Logger::Error(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Log(LogLevel::Error, L"%s", buffer);
}

void Logger::WriteToFile(const std::wstring& line) {
    EnterCriticalSection(&m_cs);
    CheckRotation();
    // If rotation just closed the handle, reopen before writing.
    if (m_hFile == INVALID_HANDLE_VALUE && !m_logPath.empty()) {
        OpenLogFile();
    }
    if (m_hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(m_hFile, line.c_str(),
                  static_cast<DWORD>(line.size() * sizeof(wchar_t)),
                  &written, nullptr);
        FlushFileBuffers(m_hFile);
    }
    LeaveCriticalSection(&m_cs);
}

namespace {
// Exact calendar-day difference via serial (Julian-day) numbers.
// All inputs are years 1601+, so no negative / special-case needed.
int DaySerial(int year, int month, int day) {
    int a = (14 - month) / 12;
    int y = year + 4800 - a;
    int m = month + 12 * a - 3;
    return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

// Path for the rotated copy of logPath: <dir>\<stem>.YYYY-MM-DD.log next to
// it, dated by the day the stale file's content started (its creation time).
std::wstring BuildDatedLogPath(const std::wstring& logPath,
                               const WIN32_FILE_ATTRIBUTE_DATA& attrs) {
    FILETIME localFt;
    FileTimeToLocalFileTime(&attrs.ftCreationTime, &localFt);
    SYSTEMTIME st;
    FileTimeToSystemTime(&localFt, &st);

    wchar_t date[16];
    _snwprintf_s(date, _TRUNCATE, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    size_t slash = logPath.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"" : logPath.substr(0, slash + 1);
    std::wstring name = (slash == std::wstring::npos) ? logPath : logPath.substr(slash + 1);
    size_t ext = (name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".log") == 0)
                     ? name.size() - 4 : name.size();
    return dir + name.substr(0, ext) + L"." + date + L".log";
}
}  // namespace

// Open m_logPath for append. FILE_WRITE_ATTRIBUTES lets us reset the creation
// time on a freshly created file: NTFS file tunneling makes a file recreated
// under a just-deleted or just-renamed name inherit the previous file's
// creation time, so without the explicit stamp a creation-time-based rotation
// would re-trigger on every write and destroy all but the newest line.
// Callers must hold m_cs.
void Logger::OpenLogFile() {
    m_hFile = CreateFileW(m_logPath.c_str(), FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE) {
        // Fall back without the extra access right; appending still works, we
        // just can't restamp the creation time.
        m_hFile = CreateFileW(m_logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        SetFileTime(m_hFile, &now, nullptr, nullptr);
    }
}

// Rotate the log file with a day-based retention window. When the current
// file started more than kMaxLogDays days ago (by its creation time), it is
// moved aside under a dated name and dated copies past the window are
// deleted, capping disk usage to roughly the last kMaxLogDays days of logs
// while keeping that history readable. Works on m_logPath directly, so it is
// safe to call before m_hFile is opened (e.g. from SetLogFile). On rotation
// the handle is closed and left INVALID; the caller reopens it. Callers must
// hold m_cs.
void Logger::CheckRotation() {
    if (m_logPath.empty())
        return;

    // Check the file on disk (independent of the open handle).
    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    if (!GetFileAttributesExW(m_logPath.c_str(), GetFileExInfoStandard, &attrs))
        return;  // file doesn't exist yet — nothing to rotate

    // Use the file's creation time to mark the day its log content starts.
    FILETIME localFt;
    FileTimeToLocalFileTime(&attrs.ftCreationTime, &localFt);
    SYSTEMTIME createdSt;
    FileTimeToSystemTime(&localFt, &createdSt);

    // Today's date.
    SYSTEMTIME nowSt;
    GetLocalTime(&nowSt);

    int days = DaySerial(nowSt.wYear, nowSt.wMonth, nowSt.wDay)
             - DaySerial(createdSt.wYear, createdSt.wMonth, createdSt.wDay);
    if (days < kMaxLogDays)
        return;  // still within the retention window

    // Close the current handle so the rename succeeds on Windows.
    if (m_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }

    // Move the stale file aside instead of deleting it, so the retention
    // window keeps history. If the move fails (e.g. another process holds the
    // file open), rotation retries on a later write and appending resumes
    // against the stale file — no lines are lost.
    // NOTE: no FACELOGIN_INFO here — it would re-enter the critical section
    // we already hold and deadlock.
    if (MoveFileExW(m_logPath.c_str(), BuildDatedLogPath(m_logPath, attrs).c_str(),
                    MOVEFILE_REPLACE_EXISTING)) {
        PurgeDatedFiles();
    }
}

// Delete rotated files next to m_logPath (<stem>.YYYY-MM-DD.log) whose start
// date is more than kMaxLogDays days before today. Strictly greater: a file
// rotated at exactly the window edge (3 days old) must survive its rotation
// day, otherwise rotation + purge would destroy the history just moved aside.
// Callers must hold m_cs.
void Logger::PurgeDatedFiles() {
    size_t slash = m_logPath.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : m_logPath.substr(0, slash);
    std::wstring name = (slash == std::wstring::npos) ? m_logPath : m_logPath.substr(slash + 1);
    size_t ext = (name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".log") == 0)
                     ? name.size() - 4 : name.size();
    std::wstring stem = name.substr(0, ext);

    SYSTEMTIME nowSt;
    GetLocalTime(&nowSt);
    int today = DaySerial(nowSt.wYear, nowSt.wMonth, nowSt.wDay);

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW((dir + L"\\" + stem + L".*.log").c_str(),
                                   FindExInfoBasic, &fd, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        // Only exact <stem>.YYYY-MM-DD.log matches (10-char date suffix).
        const wchar_t* fn = fd.cFileName;
        if (wcslen(fn) != stem.size() + 1 + 10 + 4)
            continue;
        int y = 0, m = 0, d = 0;
        if (swscanf_s(fn + stem.size() + 1, L"%4d-%2d-%2d", &y, &m, &d) != 3)
            continue;
        if (today - DaySerial(y, m, d) > kMaxLogDays)
            DeleteFileW((dir + L"\\" + fn).c_str());
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

void Logger::AppendToRingBuffer(const std::wstring& line) {
    EnterCriticalSection(&m_ringCs);
    if (m_ringBuffer.empty()) {
        m_ringBuffer.resize(RING_SIZE);
    }
    m_ringBuffer[m_ringPos] = line;
    m_ringPos = (m_ringPos + 1) % RING_SIZE;
    m_ringCount++;
    LeaveCriticalSection(&m_ringCs);
}

std::vector<std::wstring> Logger::GetRecentLogs(size_t maxLines) {
    std::vector<std::wstring> result;
    EnterCriticalSection(&m_ringCs);
    if (m_ringCount == 0) {
        LeaveCriticalSection(&m_ringCs);
        return result;
    }
    size_t count = m_ringCount;
    if (count > RING_SIZE) count = RING_SIZE;
    if (count > maxLines) count = maxLines;
    result.reserve(count);
    // Read from the oldest entry. After ring wraps, pos points to oldest.
    size_t start = (m_ringCount <= RING_SIZE) ? 0 : m_ringPos;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % RING_SIZE;
        result.push_back(m_ringBuffer[idx]);
    }
    LeaveCriticalSection(&m_ringCs);
    return result;
}

void Logger::ClearLogs() {
    EnterCriticalSection(&m_ringCs);
    m_ringBuffer.clear();
    m_ringBuffer.resize(RING_SIZE);
    m_ringPos = 0;
    m_ringCount = 0;
    LeaveCriticalSection(&m_ringCs);
}

} // namespace facelogin
