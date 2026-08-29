#pragma once

#include <windows.h>
#include <string>
#include <functional>

namespace facelogin {

// Named pipe client for the credential provider DLL.
// Connects to the FaceLogin service to send/receive authentication messages.
//
// Runs in LogonUI.exe (SYSTEM context on Secure Desktop).
// The background read thread polls the pipe (PeekNamedPipe + short ReadFile)
// so it can also watch the stop event, and delivers every message through
// callbacks — terminal results have NO second polling channel.
//
// OnResponseCallback: called from the background read thread exactly once,
// when the terminal message arrives or the pipe breaks.  The credential
// transitions state there and triggers re-enumeration.
// OnStatusCallback: called from the same thread for STATUS: messages.

using OnResponseCallback = std::function<void(bool success, const std::wstring& message)>;
using OnStatusCallback = std::function<void(const std::wstring& message)>;

class PipeClient {
public:
    PipeClient();
    ~PipeClient();

    // Non-copyable
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Connect to the FaceLogin named pipe server.
    // Retries for up to ~5 seconds (pipe server may not be ready yet).
    bool Connect(DWORD timeoutMs = 5000);

    // Send a message to the server. Returns true on success.
    bool SendMessage(const std::wstring& message);

    // Spawn a background thread that loops reading messages from the pipe.
    // STATUS: messages trigger onStatus (if set).
    // Terminal messages (AUTH_SUCCESS/AUTH_TIMEOUT/AUTH_ERROR/etc.) trigger
    // onResponse once and the thread exits.
    void StartBackgroundRead(OnResponseCallback onResponse = nullptr,
                             OnStatusCallback onStatus = nullptr);

    // Check if connected
    bool IsConnected() const { return m_connected; }

    // Close the connection (closes the pipe handle, which unblocks the
    // background read thread, then joins the thread).
    void Disconnect();

private:
    static DWORD WINAPI ReadThreadProc(LPVOID param);
    void CleanupReadThread();

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    // Written by the read thread (pipe breakage) and by the owning thread
    // (send failure / disconnect); read by IsConnected as a guard. A plain
    // bool torn read is impossible on x64 and the guard only degrades
    // gracefully, so no lock is taken around it.
    bool m_connected = false;

    // Background read
    HANDLE m_hReadThread = nullptr;
    HANDLE m_hReadStop = nullptr;        // manual-reset: signaled to stop the read thread

    // Callbacks — set before the read thread starts (CreateThread gives the
    // happens-before), never touched afterwards.
    OnResponseCallback m_onResponse;
    OnStatusCallback   m_onStatus;
};

} // namespace facelogin
