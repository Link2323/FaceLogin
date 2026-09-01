#include "pipe_client.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include <chrono>
#include <thread>
#include <cwchar>

namespace facelogin {

namespace {

constexpr DWORD kPipeWriteTimeoutMs = 5000;

bool IsPipeClosedError(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
           error == ERROR_PIPE_NOT_CONNECTED;
}

} // namespace

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
        // AUTH_SUCCESS can synchronously trigger CredentialsChanged ->
        // UnAdvise -> PipeClient destruction on this very thread. Waiting for
        // ourselves would add a two-second stall (or deadlock). The terminal
        // callback is copied to thread-local storage before invocation and the
        // thread never dereferences PipeClient afterwards, so closing only the
        // thread handle is safe in that case.
        if (GetThreadId(m_hReadThread) != GetCurrentThreadId()) {
            // Disconnect has already signalled the stop event and cancelled
            // pending I/O. Do not destroy this object until the worker has
            // reaped its OVERLAPPED operation and stopped using its members.
            WaitForSingleObject(m_hReadThread, INFINITE);
        }
        CloseHandle(m_hReadThread);
        m_hReadThread = nullptr;
    }
}

bool PipeClient::Connect(DWORD timeoutMs, const wchar_t* pipeName) {
    Disconnect();

    auto startTime = std::chrono::steady_clock::now();
    const wchar_t* name = pipeName ? pipeName : ipc::PIPE_NAME;

    while (true) {
        m_hPipe = CreateFileW(
            name,
            GENERIC_READ | GENERIC_WRITE,
            0,                          // No sharing
            nullptr,                    // Default security
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
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
            if (!WaitNamedPipeW(name, 200)) {
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

    DWORD byteSize = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));
    HANDLE writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!writeEvent) {
        FACELOGIN_ERROR(L"CreateEvent for pipe write failed: %lu", GetLastError());
        return false;
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = writeEvent;
    DWORD bytesWritten = 0;
    BOOL result = WriteFile(m_hPipe, message.c_str(), byteSize,
                            nullptr, &overlapped);
    DWORD error = result ? ERROR_SUCCESS : GetLastError();
    if (!result && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(writeEvent, kPipeWriteTimeoutMs);
        if (wait == WAIT_OBJECT_0) {
            result = GetOverlappedResult(m_hPipe, &overlapped,
                                         &bytesWritten, FALSE);
            error = result ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelIoEx(m_hPipe, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(m_hPipe, &overlapped, &ignored, TRUE);
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_GEN_FAILURE;
            result = FALSE;
        }
    } else if (result) {
        result = GetOverlappedResult(m_hPipe, &overlapped,
                                     &bytesWritten, FALSE);
        error = result ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(writeEvent);

    if (!result || bytesWritten != byteSize) {
        if (IsPipeClosedError(error)) {
            FACELOGIN_WARN(L"Pipe broken during send");
        } else if (error == ERROR_TIMEOUT) {
            FACELOGIN_WARN(L"Pipe write timed out after %lu ms", kPipeWriteTimeoutMs);
        } else {
            FACELOGIN_ERROR(L"Overlapped WriteFile on pipe failed: %lu", error);
        }
        m_connected = false;
        return false;
    }
    return true;
}

// ============================================================================
// Background read thread — loops, reading messages until a terminal one
// arrives (AUTH_SUCCESS, AUTH_TIMEOUT, etc.) or the pipe breaks.
// STATUS: messages are dispatched immediately via m_onStatus so the CP
// can update the LogonUI status text in real time.
// ============================================================================

DWORD WINAPI PipeClient::ReadThreadProc(LPVOID param) {
    PipeClient* self = static_cast<PipeClient*>(param);

    HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readEvent) {
        FACELOGIN_ERROR(L"CreateEvent for pipe read failed: %lu", GetLastError());
        self->m_connected = false;
        auto onResponse = self->m_onResponse;
        if (onResponse) onResponse(false, L"");
        return 0;
    }

    // The OVERLAPPED object and buffer are owned by this thread and remain
    // alive until every pending read has either completed or been cancelled
    // and reaped with GetOverlappedResult.
    wchar_t buffer[4096] = {};
    while (true) {
        if (WaitForSingleObject(self->m_hReadStop, 0) == WAIT_OBJECT_0) {
            FACELOGIN_INFO(L"Background read: stop signaled — exiting");
            break;
        }

        ZeroMemory(buffer, sizeof(buffer));
        ResetEvent(readEvent);
        OVERLAPPED overlapped = {};
        overlapped.hEvent = readEvent;
        DWORD bytesRead = 0;
        BOOL result = ReadFile(self->m_hPipe,
                               buffer,
                               static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                               nullptr,
                               &overlapped);
        DWORD error = result ? ERROR_SUCCESS : GetLastError();
        if (!result && error == ERROR_IO_PENDING) {
            // Prefer the stop event if completion and teardown race. A
            // terminal message received during Disconnect must not call back
            // into a credential whose owner is being destroyed.
            HANDLE waits[] = { self->m_hReadStop, readEvent };
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waits), waits,
                                                      FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                CancelIoEx(self->m_hPipe, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(self->m_hPipe, &overlapped, &ignored, TRUE);
                FACELOGIN_INFO(L"Background read: stop signaled — I/O cancelled");
                break;
            }
            if (wait == WAIT_OBJECT_0 + 1) {
                result = GetOverlappedResult(self->m_hPipe, &overlapped,
                                             &bytesRead, FALSE);
                error = result ? ERROR_SUCCESS : GetLastError();
            } else {
                CancelIoEx(self->m_hPipe, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(self->m_hPipe, &overlapped, &ignored, TRUE);
                error = GetLastError();
                result = FALSE;
            }
        } else if (result) {
            result = GetOverlappedResult(self->m_hPipe, &overlapped,
                                         &bytesRead, FALSE);
            error = result ? ERROR_SUCCESS : GetLastError();
        }

        // Disconnect may race an immediately-completed read. Teardown wins;
        // discard the completed message instead of issuing a late callback.
        if (WaitForSingleObject(self->m_hReadStop, 0) == WAIT_OBJECT_0) {
            FACELOGIN_INFO(L"Background read: stop won completion race");
            break;
        }

        const bool success = result && bytesRead > 0;
        if (!success) {
            if (IsPipeClosedError(error)) {
                FACELOGIN_INFO(L"Background read: pipe broken by server");
            } else {
                FACELOGIN_WARN(L"Background overlapped read failed: %lu", error);
            }
            self->m_connected = false;
            auto onResponse = self->m_onResponse;
            if (onResponse) onResponse(false, L"");
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

        // Terminal message — acknowledge receipt after copying it out of the
        // transport buffer. This replaces the server's unbounded
        // FlushFileBuffers delivery handshake. ACK failure does not discard a
        // terminal result already received by LogonUI; the server will time
        // out and close its side after a bounded wait.
        SecureZeroMemory(buffer, sizeof(buffer));
        if (!self->SendMessage(ipc::MSG_AUTH_ACK)) {
            FACELOGIN_WARN(L"Failed to acknowledge terminal pipe message");
        }

        // Deliver via callback, then scrub the remaining plaintext wstring
        // before this thread exits. Copy the callback first: AUTH_SUCCESS may
        // synchronously cause UnAdvise to destroy PipeClient while the
        // callback is running. Nothing below may dereference self.
        auto onResponse = self->m_onResponse;
        if (onResponse) onResponse(success, msg);
        SecureZeroMemory(msg.data(), msg.size() * sizeof(wchar_t));
        SecureZeroMemory(buffer, sizeof(buffer));
        break;
    }

    SecureZeroMemory(buffer, sizeof(buffer));
    CloseHandle(readEvent);
    return 0;
}

bool PipeClient::StartBackgroundRead(OnResponseCallback onResponse,
                                     OnStatusCallback onStatus) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return false;

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
        m_connected = false;
        return false;
    }
    return true;
}

void PipeClient::Disconnect() {
    // Signal first, then cancel the pending OVERLAPPED read while the pipe
    // handle is still valid. The read thread reaps the cancelled operation
    // before returning, so its stack OVERLAPPED and buffer cannot be released
    // while the kernel still references them.
    if (m_hReadStop) {
        SetEvent(m_hReadStop);
    }

    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_hPipe, nullptr);
    }
    m_connected = false;

    CleanupReadThread();

    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
}

} // namespace facelogin
