#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <functional>

namespace facelogin {

// Named pipe client for the credential provider DLL.
// Connects to the FaceLogin service to send/receive authentication messages.
//
// Runs in LogonUI.exe (SYSTEM context on Secure Desktop).
// The background read thread waits on an OVERLAPPED ReadFile event and the
// stop event together, so data delivery and cancellation are both immediate.
// Every message is delivered through callbacks — terminal results have NO
// second polling channel.
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
    // pipeName overrides ipc::PIPE_NAME for dev/test tools only.
    bool Connect(DWORD timeoutMs = 5000, const wchar_t* pipeName = nullptr);

    // Send a message to the server. Returns true on success.
    bool SendMessage(const std::wstring& message);

    // Spawn a background thread that loops reading messages from the pipe.
    // STATUS: messages trigger onStatus (if set).
    // Terminal messages (AUTH_SUCCESS/AUTH_TIMEOUT/AUTH_ERROR/etc.) trigger
    // onResponse once and the thread exits.
    // Returns false if the reader could not be started; no callback will run.
    bool StartBackgroundRead(OnResponseCallback onResponse = nullptr,
                             OnStatusCallback onStatus = nullptr);

    // Check if connected
    bool IsConnected() const { return m_connected; }

    // Cancel any pending read, join the background thread, then close the
    // pipe. The OVERLAPPED state and buffer stay alive until cancellation has
    // completed.
    void Disconnect();

private:
    static DWORD WINAPI ReadThreadProc(LPVOID param);
    void CleanupReadThread();

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    // Written by the read thread (pipe breakage) and by the owning thread
    // (send failure / disconnect); read concurrently by IsConnected.
    std::atomic<bool> m_connected{false};

    // Background read
    HANDLE m_hReadThread = nullptr;
    HANDLE m_hReadStop = nullptr;        // manual-reset: signaled to stop the read thread

    // Callbacks — set before the read thread starts (CreateThread gives the
    // happens-before), never touched afterwards.
    OnResponseCallback m_onResponse;
    OnStatusCallback   m_onStatus;
};

} // namespace facelogin
