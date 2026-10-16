#include "log_viewer.h"
#include "service_client.h"
#include "ipc_protocol.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using facelogin::EnrollmentLogSource;
using facelogin::ReadEnrollmentLog;
using facelogin::ClearEnrollmentLog;
namespace ipc = facelogin::ipc;
namespace fs = std::filesystem;

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(file.good(), "write test log fixture");
}

void TestLogs(const fs::path& dataDir) {
    const auto logDir = dataDir / L"log";
    std::error_code error;
    Check(fs::create_directory(logDir, error), "create fixture log directory");
    if (error) return;
    const std::wstring dir = dataDir.wstring();

    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Service) ==
              "[\"service.log not available\"]", "missing log retains placeholder");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::Service),
          "clearing a missing log succeeds");

    const std::string utf8 = "你好 \"face\" C:\\models\r\nsecond\n\n";
    WriteBytes(logDir / L"console.log", utf8);
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Console) ==
              "[\"你好 \\\"face\\\" C:\\\\models\",\"second\"]",
          "UTF-8 lines preserve Unicode and escape quotes and backslashes");

    const std::wstring legacy = L"\xfeff旧日志\r\nsecond\r\n";
    WriteBytes(logDir / L"auth_worker.log",
               std::string(reinterpret_cast<const char*>(legacy.data()),
                           legacy.size() * sizeof(wchar_t)));
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::AuthWorker) ==
              "[\"旧日志\",\"second\"]", "legacy UTF-16LE BOM log decodes");
    const std::wstring noBom = L"worker legacy\r\n";
    WriteBytes(logDir / L"auth_worker.log",
               std::string(reinterpret_cast<const char*>(noBom.data()),
                           noBom.size() * sizeof(wchar_t)));
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::AuthWorker) ==
              "[\"worker legacy\"]", "legacy UTF-16LE without BOM decodes");

    WriteBytes(logDir / L"service.log", std::string(300 * 1024, 'x') + "\r\nlatest\r\n");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Service) == "[\"latest\"]",
          "tail cap discards partial first line and retains latest complete line");
    WriteBytes(logDir / L"service.log", std::string(300 * 1024, 'x'));
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Service) == "[]",
          "oversized line without newline is not displayed as a fragment");

    WriteBytes(logDir / L"credential_provider.log", "provider\r\n");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider) ==
              "[\"provider\"]", "provider source reads the provider file");
    WriteBytes(logDir / L"console.2000-01-01.log", "archived\r\n");
    HANDLE writer = CreateFileW((logDir / L"console.log").c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "open append-mode live log writer");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::Console), "clear live console log");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Console) == "[]", "cleared log is empty");
    if (writer != INVALID_HANDLE_VALUE) {
        const std::string next = "after clear\r\n";
        DWORD written = 0;
        Check(WriteFile(writer, next.data(), static_cast<DWORD>(next.size()), &written, nullptr) &&
                  written == next.size(), "live writer appends after clear");
        CloseHandle(writer);
        Check(ReadEnrollmentLog(dir, EnrollmentLogSource::Console) == "[\"after clear\"]",
              "live append resumes at the new end without holes");
    }
    Check(fs::file_size(logDir / L"console.2000-01-01.log", error) == 10 && !error,
          "clearing active log preserves rotated archive");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider) == "[\"provider\"]",
          "clearing console leaves other sources untouched");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::AuthWorker), "clear worker source");
    Check(ClearEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider), "clear provider source");
    Check(ReadEnrollmentLog(dir, EnrollmentLogSource::AuthWorker) == "[]" &&
              ReadEnrollmentLog(dir, EnrollmentLogSource::CredentialProvider) == "[]",
          "source-specific clears target their own files");
}

// The server uses its own uniquely named pipe, never the installed service.
// Every test-side I/O has a deadline so a broken handshake cannot hang the suite.
bool CompleteIo(HANDLE pipe, OVERLAPPED& operation, BOOL immediate, DWORD& bytes) {
    if (!immediate && GetLastError() != ERROR_IO_PENDING) return false;
    if (!immediate && WaitForSingleObject(operation.hEvent, 5000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}

bool ReadMessage(HANDLE pipe, std::wstring& message) {
    wchar_t buffer[128] = {};
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    DWORD bytes = 0;
    BOOL immediate = ReadFile(pipe, buffer, sizeof(buffer), &bytes, &operation);
    bool ok = CompleteIo(pipe, operation, immediate, bytes);
    CloseHandle(operation.hEvent);
    if (ok) message.assign(buffer, bytes / sizeof(wchar_t));
    return ok;
}

bool WriteMessage(HANDLE pipe, const std::wstring& message) {
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    DWORD bytes = 0;
    const DWORD size = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));
    BOOL immediate = WriteFile(pipe, message.c_str(), size, &bytes, &operation);
    bool ok = CompleteIo(pipe, operation, immediate, bytes) && bytes == size;
    CloseHandle(operation.hEvent);
    return ok;
}

enum class Request { Database, Config, Learning };

void TestServiceRequest(Request kind, const std::wstring& reply, bool accepted,
                        bool disconnect = false) {
    static unsigned int sequence = 0;
    const std::wstring name = L"\\\\.\\pipe\\FaceLogin.EnrollmentSupportTest." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                   1, 65536, 65536, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "create isolated test pipe");
    if (pipe == INVALID_HANDLE_VALUE) return;

    std::wstring request;
    std::wstring acknowledgement;
    bool serverOk = false;
    DWORD serverError = ERROR_SUCCESS;
    std::thread server([&] {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return;
        DWORD bytes = 0;
        BOOL immediate = ConnectNamedPipe(pipe, &operation);
        bool connected = !immediate && GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) connected = CompleteIo(pipe, operation, immediate, bytes);
        CloseHandle(operation.hEvent);
        if (connected && ReadMessage(pipe, request)) {
            if (disconnect) serverOk = true;
            else serverOk = WriteMessage(pipe, reply) && ReadMessage(pipe, acknowledgement);
        }
        if (!serverOk) serverError = GetLastError();
        DisconnectNamedPipe(pipe);
    });

    facelogin::EnrollmentServiceClient client(name);
    bool result = false;
    std::string learning;
    switch (kind) {
    case Request::Database: result = client.ReloadDatabase(); break;
    case Request::Config: result = client.ReloadConfig(); break;
    case Request::Learning: learning = client.QueryLearningStatus(); break;
    }
    const DWORD clientError = GetLastError();
    server.join();
    CloseHandle(pipe);

    if (!serverOk) {
        std::fprintf(stderr, "Pipe case %u (request=%d, disconnect=%d): server error=%lu, client error=%lu, request units=%zu, ack units=%zu\n",
                     sequence, static_cast<int>(kind), disconnect, serverError, clientError,
                     request.size(), acknowledgement.size());
    }

    const wchar_t* expected = kind == Request::Database ? ipc::MSG_RELOAD_DB :
        kind == Request::Config ? ipc::MSG_CONFIG_RELOAD : ipc::MSG_LEARNING_STATUS;
    std::wstring expectedMessage(expected);
    expectedMessage.push_back(L'\0');
    Check(serverOk && request == expectedMessage, "service request uses the existing wire message and terminator");
    if (!disconnect) {
        std::wstring ack(ipc::MSG_CONTROL_ACK);
        ack.push_back(L'\0');
        Check(acknowledgement == ack, "every received response is acknowledged, including rejection");
    }
    if (kind == Request::Learning) {
        Check(learning == (disconnect ? "{\"error\":\"Service did not respond\"}" :
                                       "{\"status\":\"就绪\"}"),
              "learning query preserves JSON/UTF-8 response and disconnect error");
    } else {
        Check(result == accepted, "reload succeeds only for its matching OK response");
    }
}

void TestServiceClient() {
    TestServiceRequest(Request::Database, ipc::MSG_RELOAD_OK, true);
    TestServiceRequest(Request::Database, L"RELOAD_FAILED", false);
    TestServiceRequest(Request::Config, ipc::MSG_CONFIG_RELOAD_OK, true);
    TestServiceRequest(Request::Config, ipc::MSG_RELOAD_OK, false);
    TestServiceRequest(Request::Config, L"", false, true);
    TestServiceRequest(Request::Learning, L"{\"status\":\"就绪\"}", true);
    TestServiceRequest(Request::Learning, L"", false, true);

    facelogin::EnrollmentServiceClient absent(L"\\\\.\\pipe\\FaceLogin.AbsentEnrollmentTest." +
                                               std::to_wstring(GetCurrentProcessId()));
    Check(!absent.ReloadDatabase() && !absent.ReloadConfig(), "unavailable service cannot confirm a reload");
    Check(absent.QueryLearningStatus() == "{\"error\":\"Service not running\"}",
          "unavailable service retains learning error response");
}

} // namespace

int main() {
    std::error_code error;
    const auto tempDir = fs::temp_directory_path(error);
    if (error) return 1;
    const auto dataDir = tempDir / (L"FaceLoginEnrollmentSupportTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(dataDir, error) || error) return 1;
    TestLogs(dataDir);
    TestServiceClient();
    // Only remove the unique directory successfully created by this test.
    fs::remove_all(dataDir, error);
    Check(!error, "remove temporary test fixtures");
    std::printf("EnrollmentSupportTest: %s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures ? 1 : 0;
}
