// Dev-only: repeatedly create and close a named-pipe instance to verify the
// per-instance security descriptor's DACL is freed (no leak). Never shipped.
//
// Background: an absolute-format security descriptor built with
// InitializeSecurityDescriptor + SetSecurityDescriptorDacl does NOT own its
// DACL. The old code freed only the descriptor, leaking one SetEntriesInAclW
// ACL per pipe instance (once per auth round), so long-running lock-screen
// use made the service's private bytes grow slowly. This tool drives the real
// create/close path and asserts the leak is gone.
//
// Usage: PipeLifecycleTest [iterations] [warmup]
//   iterations  total create/close cycles (default 1000)
//   warmup      cycles to skip before the private-bytes baseline (default 100)
//
// Pass criteria:
//   1. After every Close(), PipeServer::OutstandingAclAllocations() == 0.
//   2. Private bytes at the final cycle vs the warmup cycle grow by <= 1 MiB
//      (headroom for heap fragmentation; the fix itself leaks nothing).
//   3. A pending connection wait exits in under one second when the service
//      control path calls RequestShutdown().
//   4. ReadMessage regression (unlock-flow TODO 4, closed): a connected but
//      silent client must time out instead of falling through to a blocking
//      ReadFile, the instance must then accept the next client within 250 ms,
//      a message landing just before the deadline must still be read, and
//      RequestShutdown() must cancel an in-progress read. Uses the test
//      build's shortened 1000 ms read deadline; --verify-30s additionally
//      runs the full default 30000 ms deadline.
//   5. Remote rejection (unlock-flow TODO 6, closed): PIPE_REJECT_REMOTE_CLIENTS
//      must reject transport-level remote connects. On a single dev box this
//      is exercised via loopback SMB (\\localhost / \\127.0.0.1 / \\<hostname>),
//      the same redirector path a real second machine takes.
//   6. The production credential-provider client preserves STATUS/terminal
//      ordering, cancels a pending event-driven read promptly, and can be
//      destroyed synchronously by its terminal callback without self-waiting.
//
// Uses a distinct test pipe name, so it runs even while the FaceLogin service
// is up (the service's single instance of ipc::PIPE_NAME would otherwise make
// CreateNamedPipeW fail with ERROR_PIPE_BUSY).
#include "pipe_server.h"
#include "pipe_client.h"
#include "ipc_protocol.h"

#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

#include <windows.h>
#include <psapi.h>

namespace {

constexpr unsigned long long kMiB = 1024ULL * 1024ULL;

bool ParsePositiveInt(const char* text, int& value) {
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (!text[0] || !end || *end != '\0' || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

// Private bytes (PrivateUsage lives on PROCESS_MEMORY_COUNTERS_EX, which is
// a superset of PROCESS_MEMORY_COUNTERS — pass the EX struct to
// GetProcessMemoryInfo so the PrivateUsage field is populated).
size_t PrivateUsageBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {
        return static_cast<size_t>(pmc.PrivateUsage);
    }
    return 0;
}

bool VerifyStopCancelsPipeWait() {
    facelogin::PipeServer server;
    std::atomic<bool> waitResult{true};
    const auto started = std::chrono::steady_clock::now();
    std::thread waiter([&server, &waitResult]() {
        waitResult.store(server.WaitForClient(30000));
    });

    Sleep(100);
    server.RequestShutdown();
    waiter.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    server.Close();

    if (waitResult.load() || elapsed > 1000 ||
        facelogin::PipeServer::OutstandingAclAllocations() != 0) {
        std::cerr << "FAIL: shutdown pipe wait result=" << waitResult.load()
                  << " elapsed_ms=" << elapsed
                  << " outstanding_acl="
                  << facelogin::PipeServer::OutstandingAclAllocations() << "\n";
        return false;
    }
    std::cout << "shutdown_wait_ms=" << elapsed << "\n";
    return true;
}

// A distinct pipe name so the tool runs even while the FaceLogin service is
// up (the service holds a single instance of ipc::PIPE_NAME while it waits
// for a client). The DACL includes the current interactive user, so a dev
// run can connect.
constexpr wchar_t kReadTestPipeName[] = L"\\\\.\\pipe\\FaceLoginPipeReadTest";
constexpr wchar_t kClientEventTestPipeName[] =
    L"\\\\.\\pipe\\FaceLoginPipeClientEventTest";

// The production PipeClient uses an OVERLAPPED read. Verify that the
// production service endpoint receives its overlapped write, that STATUS and
// terminal messages remain ordered when written back-to-back, and that
// Disconnect cancels a pending read without a late failure callback.
bool VerifyEventDrivenClient() {
    facelogin::PipeServer server;
    std::atomic<bool> serverOk{false};
    std::thread serverThread([&]() {
        if (!server.WaitForClient(5000, kClientEventTestPipeName)) return;
        std::wstring request;
        if (!server.ReadMessage(request, 5000) || request != L"CLIENT_EVENT_TEST") {
            return;
        }
        if (!server.WriteMessage(std::wstring(facelogin::ipc::MSG_STATUS_PREFIX) +
                                 L"event-ready")) {
            return;
        }
        if (!server.WriteMessage(facelogin::ipc::MSG_AUTH_TIMEOUT)) return;
        std::wstring ack;
        serverOk.store(server.ReadMessage(ack, facelogin::ipc::PIPE_ACK_TIMEOUT_MS) &&
                       ack == facelogin::ipc::MSG_AUTH_ACK);
    });

    Sleep(50);
    facelogin::PipeClient client;
    if (!client.Connect(5000, kClientEventTestPipeName) ||
        !client.SendMessage(L"CLIENT_EVENT_TEST")) {
        std::cerr << "FAIL: overlapped client connect/write failed\n";
        client.Disconnect();
        serverThread.join();
        server.Close();
        return false;
    }

    HANDLE terminalEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!terminalEvent) {
        std::cerr << "FAIL: terminal test event creation failed\n";
        client.Disconnect();
        serverThread.join();
        server.Close();
        return false;
    }
    std::mutex callbackMutex;
    bool statusSeen = false;
    bool orderOk = false;
    int terminalCallbacks = 0;
    if (!client.StartBackgroundRead(
        [&](bool success, const std::wstring& message) {
            std::lock_guard<std::mutex> lock(callbackMutex);
            ++terminalCallbacks;
            orderOk = success && statusSeen && message == facelogin::ipc::MSG_AUTH_TIMEOUT;
            SetEvent(terminalEvent);
        },
        [&](const std::wstring& message) {
            std::lock_guard<std::mutex> lock(callbackMutex);
            statusSeen = message == L"event-ready";
        })) {
        std::cerr << "FAIL: event client reader did not start\n";
        client.Disconnect();
        serverThread.join();
        server.Close();
        CloseHandle(terminalEvent);
        return false;
    }

    const DWORD terminalWait = WaitForSingleObject(terminalEvent, 5000);
    client.Disconnect();
    serverThread.join();
    server.Close();
    CloseHandle(terminalEvent);
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        if (terminalWait != WAIT_OBJECT_0 || !serverOk.load() || !orderOk ||
            terminalCallbacks != 1) {
            std::cerr << "FAIL: event client delivery/order terminal_wait="
                      << terminalWait << " server_ok=" << serverOk.load()
                      << " order_ok=" << orderOk
                      << " callbacks=" << terminalCallbacks << "\n";
            return false;
        }
    }

    facelogin::PipeServer cancelServer;
    HANDLE acceptedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE releaseEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!acceptedEvent || !releaseEvent) {
        if (acceptedEvent) CloseHandle(acceptedEvent);
        if (releaseEvent) CloseHandle(releaseEvent);
        std::cerr << "FAIL: cancel test event creation failed\n";
        return false;
    }
    std::thread cancelServerThread([&]() {
        if (cancelServer.WaitForClient(5000, kClientEventTestPipeName)) {
            SetEvent(acceptedEvent);
            WaitForSingleObject(releaseEvent, 5000);
        }
    });
    Sleep(50);
    facelogin::PipeClient cancelClient;
    std::atomic<int> lateCallbacks{0};
    if (!cancelClient.Connect(5000, kClientEventTestPipeName)) {
        std::cerr << "FAIL: cancel client could not connect\n";
        SetEvent(releaseEvent);
        cancelServerThread.join();
        cancelServer.Close();
        CloseHandle(acceptedEvent);
        CloseHandle(releaseEvent);
        return false;
    }
    cancelClient.StartBackgroundRead(
        [&](bool, const std::wstring&) { ++lateCallbacks; });
    if (WaitForSingleObject(acceptedEvent, 5000) != WAIT_OBJECT_0) {
        std::cerr << "FAIL: cancel server did not accept client\n";
        cancelClient.Disconnect();
        SetEvent(releaseEvent);
        cancelServerThread.join();
        cancelServer.Close();
        CloseHandle(acceptedEvent);
        CloseHandle(releaseEvent);
        return false;
    }
    Sleep(50); // ensure ReadFile is pending before cancellation
    const auto cancelStart = std::chrono::steady_clock::now();
    cancelClient.Disconnect();
    const long long cancelMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();
    SetEvent(releaseEvent);
    cancelServerThread.join();
    cancelServer.Close();
    CloseHandle(acceptedEvent);
    CloseHandle(releaseEvent);
    if (cancelMs > 250 || lateCallbacks.load() != 0) {
        std::cerr << "FAIL: pending read cancellation took " << cancelMs
                  << " ms callbacks=" << lateCallbacks.load() << "\n";
        return false;
    }

    // AUTH_SUCCESS calls CredentialsChanged, which may synchronously invoke
    // UnAdvise and destroy PipeClient on its own read thread. Reproduce that
    // ownership edge: it must neither deadlock nor pay the former 2-second
    // self-wait penalty.
    facelogin::PipeServer selfDestroyServer;
    std::thread selfDestroyServerThread([&]() {
        if (selfDestroyServer.WaitForClient(5000, kClientEventTestPipeName)) {
            if (selfDestroyServer.WriteMessage(facelogin::ipc::MSG_AUTH_TIMEOUT)) {
                std::wstring ack;
                selfDestroyServer.ReadMessage(
                    ack, facelogin::ipc::PIPE_ACK_TIMEOUT_MS);
            }
        }
    });
    Sleep(50);
    auto selfDestroyClient = std::make_unique<facelogin::PipeClient>();
    HANDLE selfDestroyedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> selfDestroyOk{false};
    if (!selfDestroyedEvent ||
        !selfDestroyClient->Connect(5000, kClientEventTestPipeName)) {
        std::cerr << "FAIL: self-destroy client setup failed\n";
        if (selfDestroyClient) selfDestroyClient->Disconnect();
        selfDestroyClient.reset();
        selfDestroyServerThread.join();
        selfDestroyServer.Close();
        if (selfDestroyedEvent) CloseHandle(selfDestroyedEvent);
        return false;
    }
    const auto selfDestroyStart = std::chrono::steady_clock::now();
    selfDestroyClient->StartBackgroundRead(
        [&](bool success, const std::wstring& message) {
            const bool terminalOk =
                success && message == facelogin::ipc::MSG_AUTH_TIMEOUT;
            selfDestroyClient.reset();
            selfDestroyOk.store(terminalOk);
            SetEvent(selfDestroyedEvent);
        });
    const DWORD selfDestroyWait = WaitForSingleObject(selfDestroyedEvent, 1000);
    const long long selfDestroyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - selfDestroyStart).count();
    if (selfDestroyClient) {
        selfDestroyClient->Disconnect();
        selfDestroyClient.reset();
    }
    selfDestroyServerThread.join();
    selfDestroyServer.Close();
    CloseHandle(selfDestroyedEvent);
    if (selfDestroyWait != WAIT_OBJECT_0 || !selfDestroyOk.load()) {
        std::cerr << "FAIL: terminal callback self-destroy wait="
                  << selfDestroyWait << " elapsed_ms=" << selfDestroyMs
                  << " result=" << selfDestroyOk.load() << "\n";
        return false;
    }
    std::cout << "event_client_cancel_ms=" << cancelMs << "\n";
    std::cout << "event_client_self_destroy_ms=" << selfDestroyMs << "\n";
    return true;
}

HANDLE ConnectTestClient() {
    HANDLE h = CreateFileW(kReadTestPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::cerr << "test client CreateFileW failed: " << GetLastError() << "\n";
    }
    return h;
}

bool WriteTestMessage(HANDLE h, const std::wstring& message) {
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));
    if (!WriteFile(h, message.c_str(), bytes, &written, nullptr) ||
        written != bytes) {
        std::cerr << "test client WriteFile failed: " << GetLastError() << "\n";
        return false;
    }
    return true;
}

// A connected-but-silent client must time out (never block on ReadFile), and
// the single instance must then accept the next client within 250 ms of the
// timeout return.
bool VerifyReadTimeoutReleasesInstance() {
    facelogin::PipeServer server;

    HANDLE silentClient = INVALID_HANDLE_VALUE;
    std::thread firstClient([&silentClient]() {
        Sleep(100);
        silentClient = ConnectTestClient();
    });
    if (!server.WaitForClient(5000, kReadTestPipeName)) {
        std::cerr << "FAIL: WaitForClient (silent client) failed\n";
        firstClient.join();
        server.Close();
        return false;
    }
    firstClient.join();
    if (silentClient == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: silent client could not connect\n";
        server.Close();
        return false;
    }

    const auto readStart = std::chrono::steady_clock::now();
    std::wstring msg;
    const bool ok = server.ReadMessage(msg, 1000);
    const long long readMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - readStart).count();
    if (ok) {
        std::cerr << "FAIL: ReadMessage succeeded for a silent client\n";
        CloseHandle(silentClient);
        server.Close();
        return false;
    }
    if (readMs < 900 || readMs > 3000) {
        std::cerr << "FAIL: ReadMessage timeout took " << readMs << " ms\n";
        CloseHandle(silentClient);
        server.Close();
        return false;
    }
    std::cout << "silent_client_read_timeout_ms=" << readMs << "\n";

    server.Disconnect();
    HANDLE secondClient = INVALID_HANDLE_VALUE;
    std::thread second([&secondClient]() {
        Sleep(50);
        secondClient = ConnectTestClient();
    });
    const auto acceptStart = std::chrono::steady_clock::now();
    const bool accepted = server.WaitForClient(5000, kReadTestPipeName);
    const long long acceptMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - acceptStart).count();
    second.join();
    if (!accepted || secondClient == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: instance not released after read timeout\n";
        CloseHandle(silentClient);
        server.Close();
        return false;
    }
    if (acceptMs > 250) {
        std::cerr << "FAIL: next client took " << acceptMs << " ms (> 250 ms)\n";
        CloseHandle(silentClient);
        CloseHandle(secondClient);
        server.Close();
        return false;
    }
    std::cout << "next_client_after_timeout_ms=" << acceptMs << "\n";

    CloseHandle(silentClient);
    CloseHandle(secondClient);
    server.Close();
    return true;
}

// A message arriving just before the deadline (900 ms with a 1000 ms
// deadline) must still be read — the post-window re-check must not turn a
// race into a spurious timeout.
bool VerifyLateMessageSucceeds() {
    facelogin::PipeServer server;

    HANDLE client = INVALID_HANDLE_VALUE;
    std::atomic<bool> clientConnected{false};
    std::thread clientThread([&client, &clientConnected]() {
        Sleep(100);
        client = ConnectTestClient();
        clientConnected.store(true);
        if (client != INVALID_HANDLE_VALUE) {
            Sleep(800);  // first byte lands at ~900 ms
            WriteTestMessage(client, L"LATE_MSG");
        }
    });
    if (!server.WaitForClient(5000, kReadTestPipeName)) {
        std::cerr << "FAIL: WaitForClient (late message) failed\n";
        clientThread.join();
        server.Close();
        return false;
    }
    // WaitForClient returns once the client's CreateFileW completes; make
    // sure the client thread has observed the handle before starting the
    // read. The write itself lands at connect + 800 ms, inside the window.
    while (!clientConnected.load()) Sleep(1);
    if (client == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: late-message client could not connect\n";
        clientThread.join();
        server.Close();
        return false;
    }

    const auto readStart = std::chrono::steady_clock::now();
    std::wstring msg;
    const bool ok = server.ReadMessage(msg, 1000);
    const long long readMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - readStart).count();
    if (!ok || msg != L"LATE_MSG") {
        std::cerr << "FAIL: late message not read (ok=" << ok << ")\n";
        CloseHandle(client);
        server.Close();
        return false;
    }
    if (readMs < 700 || readMs > 1500) {
        std::cerr << "FAIL: late message read at unexpected time ("
                  << readMs << " ms)\n";
        clientThread.join();
        CloseHandle(client);
        server.Close();
        return false;
    }
    std::cout << "late_message_read_ms=" << readMs << "\n";

    clientThread.join();
    CloseHandle(client);
    server.Close();
    return true;
}

// RequestShutdown() must cancel an in-progress read promptly (the poll loop
// observes it at short intervals), mirroring service-stop behavior.
bool VerifyStopCancelsRead() {
    facelogin::PipeServer server;

    HANDLE client = INVALID_HANDLE_VALUE;
    std::thread clientThread([&client]() {
        Sleep(100);
        client = ConnectTestClient();
    });
    if (!server.WaitForClient(5000, kReadTestPipeName)) {
        std::cerr << "FAIL: WaitForClient (stop-cancel) failed\n";
        clientThread.join();
        server.Close();
        return false;
    }
    clientThread.join();

    std::wstring msg;
    std::atomic<bool> readResult{true};
    const auto started = std::chrono::steady_clock::now();
    std::thread reader([&server, &msg, &readResult]() {
        readResult.store(server.ReadMessage(msg, 30000));
    });
    Sleep(100);
    server.RequestShutdown();
    reader.join();
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();

    const bool ok = !readResult.load() && elapsed < 1000;
    std::cout << "stop_cancels_read_ms=" << elapsed << "\n";
    if (!ok) {
        std::cerr << "FAIL: stop did not cancel read (result=" << readResult.load()
                  << " elapsed_ms=" << elapsed << ")\n";
    }
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    server.Close();
    return ok;
}

// Connects to the local machine's pipe through the SMB redirector, the same
// path a client on a real second machine takes. With
// PIPE_REJECT_REMOTE_CLIENTS the create must be rejected before the pipe
// instance ever accepts it. Returns 0 on success (BAD — flag not enforced),
// the Win32 error otherwise.
DWORD ConnectLoopbackRemote(const wchar_t* targetPath) {
    HANDLE h = CreateFileW(targetPath, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    CloseHandle(h);
    return 0;
}

// Single-machine verification of PIPE_REJECT_REMOTE_CLIENTS. A definitive
// check still needs a real second machine (see docs/unlock-flow-security-
// todo.md acceptance); this covers the transport mechanism locally.
bool VerifyRemoteRejection() {
    const wchar_t* kRejectTestPipeName = L"\\\\.\\pipe\\FaceLoginPipeRemoteRejectTest";
    facelogin::PipeServer server;

    std::atomic<bool> accepted{false};
    std::thread waiter([&server, &accepted, kRejectTestPipeName]() {
        accepted.store(server.WaitForClient(60000, kRejectTestPipeName));
    });

    // Remote forms must be rejected while the instance is listening.
    wchar_t computerName[64] = {};
    DWORD computerNameLen = 64;
    GetComputerNameW(computerName, &computerNameLen);
    const wchar_t* kRemoteTargets[] = {
        L"\\\\localhost\\pipe\\FaceLoginPipeRemoteRejectTest",
        L"\\\\127.0.0.1\\pipe\\FaceLoginPipeRemoteRejectTest",
        nullptr,  // filled with \\<hostname>\pipe\... below
    };
    wchar_t hostnamePath[128] = {};
    swprintf_s(hostnamePath, L"\\\\%s\\pipe\\FaceLoginPipeRemoteRejectTest", computerName);
    kRemoteTargets[2] = hostnamePath;

    bool rejectedAny = false;
    bool smbReachable = false;
    for (const wchar_t* target : kRemoteTargets) {
        const DWORD err = ConnectLoopbackRemote(target);
        if (err == 0) {
            std::wcerr << L"FAIL: remote-form connect SUCCEEDED via " << target << L"\n";
        } else if (err == ERROR_ACCESS_DENIED || err == ERROR_BAD_NETPATH ||
                   err == ERROR_NETWORK_ACCESS_DENIED) {
            // Rejected by the flag (access denied) or SMB unreachable (bad
            // network path); the access-denied case is the flag working.
            if (err == ERROR_ACCESS_DENIED || err == ERROR_NETWORK_ACCESS_DENIED) {
                rejectedAny = true;
                smbReachable = true;
            }
            std::wcout << L"remote_form_rejected " << target << L" err=" << err << L"\n";
        } else {
            std::wcout << L"remote_form_other " << target << L" err=" << err << L"\n";
        }
    }

    // Control: a local (\\.\pipe\...) connect must still succeed. Keep the
    // handle open until the server has accepted it — closing immediately
    // would look like a connect-and-go-away (ERROR_NO_DATA) to the server.
    HANDLE localClient = CreateFileW(kRejectTestPipeName,
                                     GENERIC_READ | GENERIC_WRITE, 0,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
    if (localClient == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: local connect failed after remote tests: "
                  << GetLastError() << "\n";
        waiter.join();
        server.Close();
        return false;
    }

    waiter.join();
    if (!accepted.load()) {
        std::cerr << "FAIL: server did not accept the local control client\n";
        CloseHandle(localClient);
        server.Close();
        return false;
    }
    CloseHandle(localClient);
    server.Close();
    if (rejectedAny) {
        std::cout << "remote_rejection=ENFORCED (access denied via loopback SMB)\n";
        return true;
    }
    if (!smbReachable) {
        std::cout << "remote_rejection=INCONCLUSIVE (loopback SMB unreachable on "
                     "this box; verify on a real second machine)\n";
        return true;
    }
    std::cerr << "FAIL: no remote form was rejected\n";
    return false;
}

// Optional full-default-deadline run (30 s): same ReadMessage code path as
// the 1000 ms test-build variant, exercising the production timeout. Run via
// --verify-30s so the default tool run stays fast.
bool VerifyDefaultDeadlineReleaseInstance() {
    facelogin::PipeServer server;

    HANDLE silentClient = INVALID_HANDLE_VALUE;
    std::thread firstClient([&silentClient]() {
        Sleep(100);
        silentClient = ConnectTestClient();
    });
    if (!server.WaitForClient(5000, kReadTestPipeName)) {
        std::cerr << "FAIL: WaitForClient (30 s silent client) failed\n";
        firstClient.join();
        server.Close();
        return false;
    }
    firstClient.join();
    if (silentClient == INVALID_HANDLE_VALUE) {
        std::cerr << "FAIL: 30 s silent client could not connect\n";
        server.Close();
        return false;
    }

    const auto readStart = std::chrono::steady_clock::now();
    std::wstring msg;
    const bool ok = server.ReadMessage(msg, 30000);
    const long long readMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - readStart).count();
    if (ok) {
        std::cerr << "FAIL: ReadMessage(30000) succeeded for a silent client\n";
        CloseHandle(silentClient);
        server.Close();
        return false;
    }
    if (readMs < 29000 || readMs > 40000) {
        std::cerr << "FAIL: ReadMessage(30000) took " << readMs << " ms\n";
        CloseHandle(silentClient);
        server.Close();
        return false;
    }
    std::cout << "default_deadline_read_timeout_ms=" << readMs << "\n";

    server.Disconnect();
    HANDLE secondClient = INVALID_HANDLE_VALUE;
    std::thread second([&secondClient]() {
        Sleep(50);
        secondClient = ConnectTestClient();
    });
    const auto acceptStart = std::chrono::steady_clock::now();
    const bool accepted = server.WaitForClient(5000, kReadTestPipeName);
    const long long acceptMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - acceptStart).count();
    second.join();
    if (!accepted || secondClient == INVALID_HANDLE_VALUE || acceptMs > 250) {
        std::cerr << "FAIL: instance not released after 30 s timeout "
                  << "(accepted=" << accepted << " accept_ms=" << acceptMs << ")\n";
        CloseHandle(silentClient);
        if (secondClient != INVALID_HANDLE_VALUE) CloseHandle(secondClient);
        server.Close();
        return false;
    }
    std::cout << "next_client_after_30s_timeout_ms=" << acceptMs << "\n";

    CloseHandle(silentClient);
    CloseHandle(secondClient);
    server.Close();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int iterations = 1000;
    int warmup = 100;
    bool verify30s = false;
    if (argc >= 2 && std::strcmp(argv[1], "--verify-30s") == 0) {
        verify30s = true;
        --argc;
        ++argv;
    }
    if (argc > 3 ||
        (argc >= 2 && !ParsePositiveInt(argv[1], iterations)) ||
        (argc == 3 && !ParsePositiveInt(argv[2], warmup))) {
        std::cerr << "usage: PipeLifecycleTest [iterations] [warmup]\n";
        return 2;
    }
    if (warmup >= iterations) {
        std::cerr << "warmup must be < iterations\n";
        return 2;
    }

    if (verify30s && !VerifyDefaultDeadlineReleaseInstance()) return 1;
    if (!VerifyEventDrivenClient()) return 1;
    if (!VerifyRemoteRejection()) return 1;
    if (!VerifyStopCancelsPipeWait()) return 1;
    if (!VerifyReadTimeoutReleasesInstance()) return 1;
    if (!VerifyLateMessageSucceeds()) return 1;
    if (!VerifyStopCancelsRead()) return 1;

    // A distinct pipe name so the tool runs even while the FaceLogin service
    // is up (the service holds a single instance of ipc::PIPE_NAME while it
    // waits for a client). The SD/ACL ownership path under test is
    // name-independent — it runs before CreateNamedPipeW.
    const wchar_t* kTestPipeName = L"\\\\.\\pipe\\FaceLoginPipeTest";

    facelogin::PipeServer server;
    size_t warmupBytes = 0;
    for (int i = 1; i <= iterations; ++i) {
        if (!server.CreatePipeInstance(30000, kTestPipeName)) {
            std::cerr << "CreatePipeInstance failed at cycle " << i << "\n";
            return 1;
        }
        server.Close();

        const long outstanding = facelogin::PipeServer::OutstandingAclAllocations();
        if (outstanding != 0) {
            std::cerr << "FAIL: " << outstanding
                      << " outstanding ACL allocations after cycle " << i
                      << "\n";
            return 1;
        }
        if (i == warmup) {
            warmupBytes = PrivateUsageBytes();
        }
    }

    const size_t endBytes = PrivateUsageBytes();
    const size_t delta = endBytes > warmupBytes ? endBytes - warmupBytes : 0;
    std::cout << "cycles=" << iterations
              << " warmup=" << warmup
              << " private_bytes_warmup=" << warmupBytes
              << " private_bytes_end=" << endBytes
              << " delta=" << delta << " bytes (limit " << kMiB << ")\n";
    if (delta > kMiB) {
        std::cerr << "FAIL: private bytes grew by " << delta
                  << " bytes (> 1 MiB) across "
                  << (iterations - warmup) << " post-warmup cycles\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}
