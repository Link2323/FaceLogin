#include "pipe_client.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include <chrono>
#include <thread>
#include <cwchar>

namespace facelogin {

PipeClient::PipeClient() {
    m_hReadStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeClient::~PipeClient() {
    Disconnect();
    if (m_hReadStop) {
        CloseHandle(m_hReadStop);
        m_hReadStop = nullptr;
    }
}

void PipeClient::CleanupReadThread() {
    if (m_hReadThread) {
        // Wait up to 2 seconds for the thread to exit.
        DWORD waitResult = WaitForSingleObject(m_hReadThread, 2000);
        if (waitResult != WAIT_OBJECT_0) {
            FACELOGIN_WARN(L"Read thread did not exit in time");
        }
        CloseHandle(m_hReadThread);
        m_hReadThread = nullptr;
    }
}

bool PipeClient::Connect(DWORD timeoutMs) {
    Disconnect();

    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        m_hPipe = CreateFileW(
            ipc::PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,                          // No sharing
            nullptr,                    // Default security
            OPEN_EXISTING,
            0,                          // Synchronous I/O
            nullptr);

        if (m_hPipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(m_hPipe, &mode, nullptr, nullptr)) {
                FACELOGIN_WARN(L"SetNamedPipeHandleState failed: %lu", GetLastError());
            }

            m_connected = true;
            return true;
        }

        DWORD err = GetLastError();

        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(ipc::PIPE_NAME, 200)) {
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeoutMs) {
                    FACELOGIN_WARN(L"Pipe connection timed out (pipe busy)");
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        if (err == ERROR_FILE_NOT_FOUND) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeoutMs) {
                FACELOGIN_WARN(L"Pipe connection timed out (pipe not found)");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        FACELOGIN_ERROR(L"CreateFile on pipe failed: %lu", err);
        return false;
    }
}

bool PipeClient::SendMessage(const std::wstring& message) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    DWORD byteSize = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));

    BOOL result = WriteFile(m_hPipe, message.c_str(), byteSize,
                            &bytesWritten, nullptr);

    if (!result) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            FACELOGIN_WARN(L"Pipe broken during send");
            m_connected = false;
        } else {
            FACELOGIN_ERROR(L"WriteFile on pipe failed: %lu", err);
        }
        return false;
    }

    return bytesWritten == byteSize;
}

// ============================================================================
// Background read thread — loops, reading messages until a terminal one
// arrives (AUTH_SUCCESS, AUTH_TIMEOUT, etc.) or the pipe breaks.
// STATUS: messages are dispatched immediately via m_onStatus so the CP
// can update the LogonUI status text in real time.
// ============================================================================

DWORD WINAPI PipeClient::ReadThreadProc(LPVOID param) {
    PipeClient* self = static_cast<PipeClient*>(param);

    // Poll the pipe for data instead of blocking forever on ReadFile.
    // A permanently-blocked synchronous ReadFile is NOT canceled by
    // CloseHandle (cancel-on-close only works for OVERLAPPED I/O), so a
    // thread stuck there would keep the pipe's client file object alive
    // and the server could never observe the disconnect. We instead check
    // PeekNamedPipe + the stop event in a short loop and exit promptly.
    wchar_t buffer[4096];
    while (true) {
        // Stop signal (set by Disconnect) — exit without touching the pipe.
        if (WaitForSingleObject(self->m_hReadStop, 0) == WAIT_OBJECT_0) {
            FACELOGIN_INFO(L"Background read: stop signaled — exiting");
            break;
        }

        DWORD bytesAvail = 0, totalBytes = 0;
        if (PeekNamedPipe(self->m_hPipe, nullptr, 0, nullptr, &bytesAvail, &totalBytes)) {
            if (bytesAvail == 0) {
                // Connected but idle — wait briefly, keep polling.
                Sleep(50);
                continue;
            }
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
                err == ERROR_PIPE_NOT_CONNECTED) {
                FACELOGIN_INFO(L"Background read: pipe broken by server");
            } else {
                FACELOGIN_WARN(L"Background read failed: %lu", err);
            }
            // Notify — connection lost unexpectedly
            self->m_connected = false;
            if (self->m_onResponse) {
                self->m_onResponse(false, L"");
            }
            break;
        }

        // Data is available — ReadFile returns immediately.
        ZeroMemory(buffer, sizeof(buffer));
        DWORD bytesRead = 0;
        BOOL result = ReadFile(self->m_hPipe,
                               buffer,
                               static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                               &bytesRead,
                               nullptr);

        bool success = (result && bytesRead > 0);
        if (!success) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                FACELOGIN_INFO(L"Background read: pipe broken by server");
            } else {
                FACELOGIN_WARN(L"Background read failed: %lu", err);
            }
            self->m_connected = false;
            if (self->m_onResponse) {
                self->m_onResponse(false, L"");
            }
            break;
        }

        // Parse message from buffer
        size_t len = bytesRead / sizeof(wchar_t);
        while (len > 0 && buffer[len - 1] == L'\0') {
            len--;
        }
        std::wstring msg(buffer, len);
        // STATUS: prefix → dispatch immediately, keep reading. This receipt
        // line is the single log per status message. Terminal messages get no
        // line here — OnPipeResponse logs each outcome semantically, which
        // also keeps AUTH_SUCCESS payloads (plaintext password) out of the
        // log without a special-cased redaction branch.
        if (msg.starts_with(ipc::MSG_STATUS_PREFIX)) {
            std::wstring statusText = msg.substr(wcslen(ipc::MSG_STATUS_PREFIX));
            FACELOGIN_INFO(L"Background read received: STATUS:%s (len=%zu)",
                           statusText.substr(0, 80).c_str(), len);
            if (self->m_onStatus) {
                self->m_onStatus(statusText);
            }
            continue;  // keep looping for more messages
        }

        // Terminal message — deliver via callback, then scrub both plaintext
        // copies (the wstring heap buffer and the stack buffer) before this
        // thread exits. The credential has already copied what it needs.
        if (self->m_onResponse) {
            self->m_onResponse(success, msg);
        }
        SecureZeroMemory(msg.data(), msg.size() * sizeof(wchar_t));
        SecureZeroMemory(buffer, sizeof(buffer));
        break;
    }

    return 0;
}

void PipeClient::StartBackgroundRead(OnResponseCallback onResponse,
                                      OnStatusCallback onStatus) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return;

    // Join any previous thread
    CleanupReadThread();

    // Reset state
    ResetEvent(m_hReadStop);
    m_onResponse = std::move(onResponse);
    m_onStatus   = std::move(onStatus);

    m_hReadThread = CreateThread(
        nullptr, 0,
        ReadThreadProc, this,
        0, nullptr);

    if (!m_hReadThread) {
        FACELOGIN_ERROR(L"Failed to create read thread: %lu", GetLastError());
    }
}

void PipeClient::Disconnect() {
    // Signal the read thread to stop FIRST. It polls the stop event every
    // ~50ms, so it exits promptly. (A permanently-blocked synchronous ReadFile
    // would NOT be canceled by CloseHandle — cancel-on-close only works for
    // OVERLAPPED I/O — leaving a "zombie" thread that keeps the pipe's client
    // file object alive, so the server never sees the disconnect.)
    if (m_hReadStop) {
        SetEvent(m_hReadStop);
    }

    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
    m_connected = false;

    CleanupReadThread();
}

} // namespace facelogin
