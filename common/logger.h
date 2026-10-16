#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cstdint>

namespace facelogin {

// Simple file/console logger for debugging.
// In production (credential provider), messages go to a log file.
// In test/CLI programs, messages go to stdout or DebugOutput.
// Log files are UTF-8 without BOM (directly readable by grep/git bash/PowerShell).

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger& Instance();

    void SetLogFile(const std::wstring& path);
    void SetMinLevel(LogLevel level);
    void SetEnableDebugOutput(bool enable) { m_debugOutput = enable; }

    void Log(LogLevel level, const wchar_t* format, ...);
    void Debug(const wchar_t* format, ...);
    void Info(const wchar_t* format, ...);
    void Warning(const wchar_t* format, ...);
    void Error(const wchar_t* format, ...);

    // In-memory ring buffer for UI log viewer
    std::vector<std::wstring> GetRecentLogs(size_t maxLines = 500);
    void ClearLogs();

private:
    Logger() = default;
    // Closes the log handle and destroys the critical sections. Static
    // destruction runs when the host unloads the DLL (installer in-process
    // self-reg via FreeLibrary) or exits; without an explicit close, the
    // handle dangles until process exit and blocks the installer's own file
    // deletion minutes later (delete deferred to reboot).
    ~Logger();
    void WriteToFile(const std::wstring& line, bool flushToDisk);
    void AppendToRingBuffer(const std::wstring& line);

    HANDLE m_hFile = INVALID_HANDLE_VALUE;
    std::wstring m_logPath;        // current log file path (for rotation)
    LogLevel m_minLevel = LogLevel::Info;
    bool m_debugOutput = false;
    CRITICAL_SECTION m_cs{};
    bool m_csInitialized = false;

    // Log rotation with a day-based retention window: when the current file
    // started more than kMaxLogDays days ago, it is moved aside under a dated
    // name (<name>.YYYY-MM-DD.log) and a fresh file is started, so roughly the
    // last kMaxLogDays days of logs stay on disk. A fresh file's creation time
    // is set explicitly on open — otherwise NTFS file tunneling (a file
    // recreated under a just-deleted/renamed name inherits the old creation
    // time) keeps the stale birth date alive and re-triggers rotation on every
    // write, destroying all but the newest line.
    static constexpr int kMaxLogDays = 14;  // progressive-learning calibration needs a ≥2-week window; volume is ~1 MB/day

    void CheckRotation();   // rotate if m_logPath is stale (older than kMaxLogDays)
    void MaybeCheckRotation();  // throttled CheckRotation for the per-write path
    void OpenLogFile();     // open m_logPath for append, stamp creation time on fresh files
    void PurgeDatedFiles(); // delete rotated <name>.<date>.log files past the window
    void RotateAsideLegacyUtf16();  // one-time UTF-8 switch: rename a pre-switch UTF-16LE log to its dated name

    // Rotation is day-granularity, so re-reading the file's creation time on
    // every write (a metadata syscall per line) is waste: the per-write path
    // re-checks at most this often. SetLogFile always checks once at startup,
    // so a file that crosses the window mid-run simply rotates a few seconds
    // late on the next check.
    static constexpr ULONGLONG kRotationCheckIntervalMs = 30000;
    ULONGLONG m_lastRotationCheck = 0;

    // Ring buffer for UI log viewer (cursor wraps when full)
    static constexpr size_t RING_SIZE = 2000;
    std::vector<std::wstring> m_ringBuffer;
    size_t m_ringPos = 0;          // next write position
    size_t m_ringCount = 0;        // total entries written
    CRITICAL_SECTION m_ringCs{};
    bool m_ringCsInitialized = false;
};

// Convenience macros for file/line info
#define FACELOGIN_LOG(level, fmt, ...) \
    facelogin::Logger::Instance().Log(level, L"[%s:%d] " fmt, __FUNCTIONW__, __LINE__, ##__VA_ARGS__)

#define FACELOGIN_DEBUG(fmt, ...) FACELOGIN_LOG(facelogin::LogLevel::Debug, fmt, ##__VA_ARGS__)
#define FACELOGIN_INFO(fmt, ...)  FACELOGIN_LOG(facelogin::LogLevel::Info, fmt, ##__VA_ARGS__)
#define FACELOGIN_WARN(fmt, ...)  FACELOGIN_LOG(facelogin::LogLevel::Warning, fmt, ##__VA_ARGS__)
#define FACELOGIN_ERROR(fmt, ...) FACELOGIN_LOG(facelogin::LogLevel::Error, fmt, ##__VA_ARGS__)

} // namespace facelogin
