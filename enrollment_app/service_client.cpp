#include "service_client.h"
#include "../common/ipc_protocol.h"
#include "../common/logger.h"
#include "../common/text_util.h"

#include <utility>

namespace facelogin {

EnrollmentServiceClient::EnrollmentServiceClient()
    : EnrollmentServiceClient(ipc::PIPE_NAME) {}

EnrollmentServiceClient::EnrollmentServiceClient(std::wstring pipeName)
    : m_pipeName(std::move(pipeName)) {}

static bool SendControlAck(HANDLE hPipe) {
    std::wstring ack(ipc::MSG_CONTROL_ACK);
    ack.push_back(L'\0');
    const DWORD expected = static_cast<DWORD>(ack.size() * sizeof(wchar_t));
    DWORD written = 0;
    return WriteFile(hPipe, ack.c_str(), expected, &written, nullptr) &&
           written == expected;
}

// Outcome of one request/response round trip. The empty string is never a
// legitimate reply on these message types, so `ok` false always means the
// exchange failed with `response` empty and `error` holding the Windows
// error for the caller's retry/context logging; `ok` true hands the
// response to the caller for interpretation.
struct RequestAckOutcome {
    bool ok = false;
    std::wstring response;
    DWORD error = ERROR_SUCCESS;
};

// One request/response round trip on a connected public pipe: write the
// request, poll up to replyTimeoutMs for the reply, read the complete
// response message, then acknowledge with CONTROL_ACK so the service can
// disconnect. Callers own the handle (message mode, retries, closing).
static RequestAckOutcome RequestAckRoundTrip(HANDLE hPipe,
                                              const std::wstring& request,
                                              DWORD replyTimeoutMs,
                                              DWORD pollStepMs,
                                              const wchar_t* ackContext) {
    RequestAckOutcome outcome;

    std::wstring message(request);
    message.push_back(L'\0');
    const DWORD expectedBytes =
        static_cast<DWORD>(message.size() * sizeof(wchar_t));
    DWORD written = 0;
    if (!WriteFile(hPipe, message.c_str(), expectedBytes, &written, nullptr)) {
        outcome.error = GetLastError();
        return outcome;
    }
    if (written != expectedBytes) {
        outcome.error = ERROR_WRITE_FAULT;
        return outcome;
    }

    DWORD bytesAvailable = 0;
    bool pipeBroken = false;
    for (DWORD waited = 0; waited < replyTimeoutMs; waited += pollStepMs) {
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
            outcome.error = GetLastError();
            pipeBroken = true;
            break;
        }
        if (bytesAvailable > 0) break;
        Sleep(pollStepMs);
    }
    if (pipeBroken) return outcome;
    if (bytesAvailable == 0) {
        outcome.error = ERROR_TIMEOUT;
        return outcome;
    }

    // Message-mode pipe: a message larger than the read buffer returns
    // ERROR_MORE_DATA with a partial chunk; keep reading until ReadFile
    // reports the message complete.
    std::wstring response;
    wchar_t buffer[4096];
    DWORD bytesRead = 0;
    for (;;) {
        const BOOL ok = ReadFile(hPipe, buffer,
                                 static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                                 &bytesRead, nullptr);
        if (ok || GetLastError() == ERROR_MORE_DATA) {
            response.append(buffer, bytesRead / sizeof(wchar_t));
            if (ok) break;
            continue;
        }
        outcome.error = GetLastError();
        return outcome;
    }
    while (!response.empty() && response.back() == L'\0') {
        response.pop_back();
    }
    if (response.empty()) {
        outcome.error = ERROR_READ_FAULT;
        return outcome;
    }

    if (!SendControlAck(hPipe)) {
        FACELOGIN_WARN(L"Failed to acknowledge %s response: %lu",
                       ackContext, GetLastError());
    }
    outcome.ok = true;
    outcome.response = std::move(response);
    return outcome;
}

// Notify the FaceLogin service to reload the user database after a write and
// keep the connection alive until the service acknowledges RELOAD_OK. The
// service accepts connections with an event-driven ConnectNamedPipe and waits
// for CONTROL_ACK before disconnecting, so both directions have an explicit,
// bounded delivery handshake.
bool EnrollmentServiceClient::ReloadDatabase() const {
    constexpr DWORD kAttempts = 5;
    constexpr DWORD kBusyWaitMs = 1000;
    constexpr DWORD kReplyTimeoutMs = 5000;

    DWORD lastError = ERROR_SUCCESS;
    for (DWORD attempt = 1; attempt <= kAttempts; ++attempt) {
        HANDLE hPipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            lastError = GetLastError();
            if (lastError == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(m_pipeName.c_str(), kBusyWaitMs);
                continue;
            }
            FACELOGIN_ERROR(L"Database reload connection failed (attempt %lu/%lu): %lu",
                            attempt, kAttempts, lastError);
            break;
        }

        DWORD readMode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(hPipe, &readMode, nullptr, nullptr);

        RequestAckOutcome outcome =
            RequestAckRoundTrip(hPipe, ipc::MSG_RELOAD_DB, kReplyTimeoutMs, 25,
                                L"database reload");
        CloseHandle(hPipe);
        if (outcome.ok) {
            if (outcome.response == ipc::MSG_RELOAD_OK) {
                FACELOGIN_INFO(L"Service confirmed database reload");
                return true;
            }
            FACELOGIN_ERROR(L"Service rejected database reload: %s",
                            outcome.response.c_str());
            return false;
        }
        lastError = outcome.error;
        FACELOGIN_WARN(L"Database reload round trip failed (attempt %lu/%lu): %lu",
                       attempt, kAttempts, lastError);
        Sleep(50);
    }

    FACELOGIN_ERROR(L"users.dat was saved but the service did not confirm reload: %lu",
                    lastError);
    return false;
}

// Learning-observability query. Unlike the log views (which read files to
// stay off the single-instance pipe), this needs live learner state, so it
// takes the public pipe for one short request/response round trip. The view
// throttles to one query per 10s for the same reason.
std::string EnrollmentServiceClient::QueryLearningStatus() const {
    HANDLE hPipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        return "{\"error\":\"Service not running\"}";
    }
    // The service assembles the response synchronously — no model or worker
    // work happens on this path — so a short bounded wait is enough.
    RequestAckOutcome outcome =
        RequestAckRoundTrip(hPipe, ipc::MSG_LEARNING_STATUS, 3000, 50,
                            L"learning status");
    CloseHandle(hPipe);
    if (!outcome.ok) {
        return "{\"error\":\"Service did not respond\"}";
    }
    return WstrToUtf8(outcome.response);
}

bool EnrollmentServiceClient::ReloadConfig() const {
    HANDLE hPipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"Configuration saved but service is unavailable for reload: %lu",
                        GetLastError());
        return false;
    }
    // A service-mode CONFIG_RELOAD can replace a preloaded worker so the
    // selected camera backend takes effect before acknowledging. Allow its
    // bounded startup handshake to complete.
    RequestAckOutcome outcome =
        RequestAckRoundTrip(hPipe, ipc::MSG_CONFIG_RELOAD, 25000, 50,
                            L"configuration reload");
    CloseHandle(hPipe);
    if (!outcome.ok) {
        return false;
    }
    if (outcome.response != ipc::MSG_CONFIG_RELOAD_OK) {
        FACELOGIN_ERROR(L"Service rejected configuration reload: %s",
                        outcome.response.c_str());
        return false;
    }
    return true;
}

} // namespace facelogin
