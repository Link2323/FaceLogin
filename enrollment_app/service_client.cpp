#include "service_client.h"
#include "enrollment_text.h"
#include "../common/ipc_protocol.h"
#include "../common/logger.h"

#include <utility>

namespace facelogin {

using enrollment_detail::WstrToUtf8;

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

        std::wstring message(ipc::MSG_RELOAD_DB);
        message.push_back(L'\0');
        const DWORD expectedBytes =
            static_cast<DWORD>(message.size() * sizeof(wchar_t));
        DWORD written = 0;
        const BOOL writeOk = WriteFile(hPipe, message.c_str(), expectedBytes,
                                       &written, nullptr);
        if (!writeOk || written != expectedBytes) {
            lastError = writeOk ? ERROR_WRITE_FAULT : GetLastError();
            FACELOGIN_WARN(L"Database reload write failed (attempt %lu/%lu): %lu",
                           attempt, kAttempts, lastError);
            CloseHandle(hPipe);
            Sleep(50);
            continue;
        }

        DWORD bytesAvailable = 0;
        bool pipeBroken = false;
        for (DWORD waited = 0; waited < kReplyTimeoutMs; waited += 25) {
            if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
                lastError = GetLastError();
                pipeBroken = true;
                break;
            }
            if (bytesAvailable > 0) break;
            Sleep(25);
        }

        if (!pipeBroken && bytesAvailable > 0) {
            wchar_t responseBuffer[64] = {};
            DWORD bytesRead = 0;
            if (ReadFile(hPipe, responseBuffer,
                         static_cast<DWORD>(sizeof(responseBuffer) - sizeof(wchar_t)),
                         &bytesRead, nullptr) && bytesRead > 0) {
                size_t responseLength = bytesRead / sizeof(wchar_t);
                while (responseLength > 0 &&
                       responseBuffer[responseLength - 1] == L'\0') {
                    --responseLength;
                }
                const std::wstring response(responseBuffer, responseLength);
                if (!SendControlAck(hPipe)) {
                    FACELOGIN_WARN(L"Failed to acknowledge database reload response: %lu",
                                   GetLastError());
                }
                CloseHandle(hPipe);
                if (response == ipc::MSG_RELOAD_OK) {
                    FACELOGIN_INFO(L"Service confirmed database reload");
                    return true;
                }
                FACELOGIN_ERROR(L"Service rejected database reload: %s",
                                response.c_str());
                return false;
            }
            lastError = GetLastError();
        } else if (!pipeBroken) {
            lastError = ERROR_TIMEOUT;
        }

        FACELOGIN_WARN(L"Database reload acknowledgement failed (attempt %lu/%lu): %lu",
                       attempt, kAttempts, lastError);
        CloseHandle(hPipe);
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
    std::string result = "{\"error\":\"Service did not respond\"}";
    DWORD written = 0;
    std::wstring msg(ipc::MSG_LEARNING_STATUS);
    msg.push_back(L'\0');
    const DWORD expectedBytes = static_cast<DWORD>(msg.size() * sizeof(wchar_t));
    if (WriteFile(hPipe, msg.c_str(), expectedBytes, &written, nullptr) &&
        written == expectedBytes) {
        // The service assembles the response synchronously — no model or
        // worker work happens on this path — so a short bounded wait is
        // enough (mirrors SetConfig's handshake shape).
        DWORD bytesAvailable = 0;
        for (DWORD waited = 0; waited < 3000; waited += 50) {
            if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
                break;
            }
            if (bytesAvailable > 0) break;
            Sleep(50);
        }
        if (bytesAvailable > 0) {
            std::wstring response;
            wchar_t buffer[8192];
            DWORD bytesRead = 0;
            // Message-mode pipe: a message larger than the read buffer
            // returns ERROR_MORE_DATA with a partial chunk; keep reading
            // until ReadFile reports the message complete.
            for (;;) {
                const BOOL ok = ReadFile(hPipe, buffer,
                                         static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                                         &bytesRead, nullptr);
                if (ok || GetLastError() == ERROR_MORE_DATA) {
                    response.append(buffer, bytesRead / sizeof(wchar_t));
                    if (ok) break;
                    continue;
                }
                response.clear();
                break;
            }
            while (!response.empty() && response.back() == L'\0') {
                response.pop_back();
            }
            if (!response.empty()) {
                if (!SendControlAck(hPipe)) {
                    FACELOGIN_WARN(L"Failed to acknowledge learning status response: %lu",
                                   GetLastError());
                }
                result = WstrToUtf8(response);
            }
        }
    }
    CloseHandle(hPipe);
    return result;
}

bool EnrollmentServiceClient::ReloadConfig() const {
    bool reloadConfirmed = false;
    HANDLE hPipe = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        std::wstring msg(ipc::MSG_CONFIG_RELOAD);
        msg.push_back(L'\0');
        const DWORD expectedBytes = static_cast<DWORD>(msg.size() * sizeof(wchar_t));
        BOOL writeOk = WriteFile(hPipe, msg.c_str(), expectedBytes, &written, nullptr);

        if (writeOk && written == expectedBytes) {
            DWORD bytesAvailable = 0;
            // A service-mode CONFIG_RELOAD can replace a preloaded worker so
            // the selected camera backend takes effect before acknowledging.
            // Allow its bounded startup handshake to complete.
            for (DWORD waited = 0; waited < 25000; waited += 50) {
                if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
                    break;
                }
                if (bytesAvailable > 0) break;
                Sleep(50);
            }

            if (bytesAvailable > 0) {
                wchar_t responseBuffer[64] = {};
                DWORD bytesRead = 0;
                if (ReadFile(hPipe, responseBuffer,
                             static_cast<DWORD>(sizeof(responseBuffer) - sizeof(wchar_t)),
                             &bytesRead, nullptr) && bytesRead > 0) {
                    size_t responseLength = bytesRead / sizeof(wchar_t);
                    while (responseLength > 0 && responseBuffer[responseLength - 1] == L'\0') {
                        responseLength--;
                    }
                    std::wstring response(responseBuffer, responseLength);
                    if (!SendControlAck(hPipe)) {
                        FACELOGIN_WARN(L"Failed to acknowledge configuration reload response: %lu",
                                       GetLastError());
                    }
                    reloadConfirmed = (response == ipc::MSG_CONFIG_RELOAD_OK);
                    if (!reloadConfirmed) {
                        FACELOGIN_ERROR(L"Service rejected configuration reload: %s",
                                        response.c_str());
                    }
                }
            }
        }
        CloseHandle(hPipe);
    } else {
        FACELOGIN_ERROR(L"Configuration saved but service is unavailable for reload: %lu",
                        GetLastError());
    }

    return reloadConfirmed;
}

} // namespace facelogin
