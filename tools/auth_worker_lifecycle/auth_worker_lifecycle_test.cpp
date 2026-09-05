#include "auth_worker_client.h"
#include "auth_worker_protocol.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using facelogin::auth_worker::Channel;
using facelogin::auth_worker::Message;
using facelogin::auth_worker::MessageType;
using facelogin::auth_worker::ReadStatus;

int g_failures = 0;

std::wstring CurrentExecutablePath();

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

bool ParseUnsignedHandle(const wchar_t* text, HANDLE& handle) {
    if (!text || !text[0]) return false;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (!end || *end != L'\0' || value == 0) return false;
    handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
    return true;
}

bool ParseWorkerHandles(int argc, wchar_t** argv, HANDLE& input, HANDLE& output) {
    if (argc != 6 || std::wstring(argv[1]) != L"-auth-worker") return false;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::wstring flag = argv[i];
        if (flag == L"--in") {
            if (!ParseUnsignedHandle(argv[i + 1], input)) return false;
        } else if (flag == L"--out") {
            if (!ParseUnsignedHandle(argv[i + 1], output)) return false;
        } else {
            return false;
        }
    }
    return input && output;
}

std::vector<float> UnitEmbedding() {
    using namespace facelogin::auth_worker;
    const float value = 1.0f / std::sqrt(static_cast<float>(kEmbeddingDimension));
    return std::vector<float>(kEmbeddingDimension, value);
}

[[noreturn]] void ExitMock(DWORD code) {
    TerminateProcess(GetCurrentProcess(), code);
    ExitProcess(code);
}

int RunMockWorker(HANDLE input, HANDLE output) {
    Channel channel(input, output);
    if (!channel.IsValid() || !channel.Write(MessageType::Hello, 0)) {
        return ERROR_BROKEN_PIPE;
    }

    Message init;
    if (channel.Read(init, 5000) != ReadStatus::Ok ||
        init.type != MessageType::Init || init.requestId != 0) {
        return ERROR_INVALID_DATA;
    }
    facelogin::auth_worker::WorkerConfig config;
    if (!facelogin::auth_worker::DecodeConfig(init.payload, config)) {
        return ERROR_INVALID_DATA;
    }
    const std::wstring mode = config.cameraDevice;
    if (mode == L"mock:crash-load") ExitMock(ERROR_BAD_EXE_FORMAT);
    if (mode == L"mock:model-fail") {
        channel.Write(MessageType::Fatal, 0,
                      facelogin::auth_worker::EncodeWString(L"mock model failure"));
        return ERROR_FILE_NOT_FOUND;
    }
    if (!channel.Write(MessageType::Ready, 0)) return ERROR_BROKEN_PIPE;

    Message start;
    for (;;) {
        const ReadStatus status = channel.Read(start, 1000);
        if (status == ReadStatus::Timeout) continue;
        if (status != ReadStatus::Ok) return ERROR_BROKEN_PIPE;
        if (start.type == MessageType::Cancel) return ERROR_SUCCESS;
        if (start.type != MessageType::StartAuth || start.requestId == 0) {
            return ERROR_INVALID_DATA;
        }
        break;
    }

    if (mode == L"mock:hang") {
        for (;;) Sleep(1000);
    }
    if (mode == L"mock:crash-camera") ExitMock(ERROR_DEVICE_NOT_AVAILABLE);
    if (mode == L"mock:disconnect") {
        channel.Close();
        return ERROR_BROKEN_PIPE;
    }
    if (mode == L"mock:wrong-request") {
        channel.Write(MessageType::Status, start.requestId + 1,
                      facelogin::auth_worker::EncodeWString(L"wrong request"));
        for (;;) Sleep(1000);
    }
    if (!channel.Write(MessageType::Status, start.requestId,
                       facelogin::auth_worker::EncodeWString(L"mock running"))) {
        return ERROR_BROKEN_PIPE;
    }
    if (mode == L"mock:crash-pad") ExitMock(ERROR_PROCESS_ABORTED);
    if (mode == L"mock:bad-embedding") {
        channel.Write(MessageType::MatchProbe, start.requestId,
            facelogin::auth_worker::EncodeMatchProbe(0,
                std::vector<float>(facelogin::auth_worker::kEmbeddingDimension, 0.0f),
                21.0f, 0.0f, 0.0f));
        for (;;) Sleep(1000);
    }

    const auto embedding = UnitEmbedding();
    for (unsigned int binding = 0; binding < 3; ++binding) {
        if (mode == L"mock:crash-binding3" && binding == 2) {
            ExitMock(ERROR_PROCESS_ABORTED);
        }
        unsigned int wireBinding = binding;
        if (mode == L"mock:binding-skip" && binding == 0) wireBinding = 1;
        if (mode == L"mock:binding-repeat" && binding == 1) wireBinding = 0;
        if (!channel.Write(MessageType::MatchProbe, start.requestId,
                           facelogin::auth_worker::EncodeMatchProbe(
                               wireBinding, embedding, 21.0f, 0.0f, 0.0f))) {
            return ERROR_BROKEN_PIPE;
        }
        Message decision;
        if (channel.Read(decision, 5000, 1) != ReadStatus::Ok ||
            decision.requestId != start.requestId ||
            decision.type != MessageType::MatchAccept ||
            !decision.payload.empty()) {
            return ERROR_INVALID_DATA;
        }
    }
    std::vector<uint8_t> successPayload =
        facelogin::auth_worker::EncodeAuthTiming({12.5f, 34.5f, 47.0f});
    if (mode == L"mock:bad-success-timing") successPayload.clear();
    if (!channel.Write(MessageType::AuthSucceeded, start.requestId,
                       successPayload)) {
        return ERROR_BROKEN_PIPE;
    }
    ExitMock(ERROR_SUCCESS);
}

facelogin::auth_worker::WorkerConfig MockConfig(const wchar_t* mode) {
    facelogin::auth_worker::WorkerConfig config;
    config.cameraDevice = mode;
    return config;
}

facelogin::AuthWorkerCallbacks AcceptCallbacks(unsigned int& bindings,
                                               bool cancel = false) {
    facelogin::AuthWorkerCallbacks callbacks;
    callbacks.reportStatus = [](const std::wstring&) {};
    callbacks.isCancelled = [cancel]() { return cancel; };
    callbacks.verifyBinding = [&bindings](const std::vector<float>& embedding,
                                          unsigned int index, float, float, float) {
        if (embedding.size() != facelogin::auth_worker::kEmbeddingDimension ||
            index != bindings) {
            return facelogin::BindingDecision{
                facelogin::BindingDecisionKind::Reject, L"bad mock binding"};
        }
        ++bindings;
        return facelogin::BindingDecision{facelogin::BindingDecisionKind::Accept, {}};
    };
    return callbacks;
}

struct Metrics {
    DWORD handles = 0;
    DWORD threads = 0;
    SIZE_T privateBytes = 0;
    SIZE_T workingSetBytes = 0;
};

DWORD CountThreads(DWORD processId) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    DWORD count = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == processId) ++count;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

bool ReadMetrics(Metrics& metrics) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessHandleCount(GetCurrentProcess(), &metrics.handles) ||
        !GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) return false;
    metrics.threads = CountThreads(GetCurrentProcessId());
    metrics.privateBytes = counters.PrivateUsage;
    metrics.workingSetBytes = counters.WorkingSetSize;
    return true;
}

std::vector<DWORD> ChildProcessIds(DWORD parentId) {
    std::vector<DWORD> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return children;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parentId) {
                children.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return children;
}

void TestStartupFailures() {
    std::wstring error;
    facelogin::AuthWorkerClient missingExecutable(
        MockConfig(L"mock:success"), 1000, CurrentExecutablePath() + L".missing");
    Check(!missingExecutable.Start(error) && !error.empty(),
          "missing worker executable is surfaced and inherited handles are reclaimed");

    facelogin::AuthWorkerClient modelFailure(MockConfig(L"mock:model-fail"));
    error.clear();
    Check(!modelFailure.Start(error) && !error.empty(),
          "mock model-load failure is surfaced and reclaimed");
    facelogin::AuthWorkerClient startupCrash(MockConfig(L"mock:crash-load"));
    error.clear();
    Check(!startupCrash.Start(error) && !error.empty(),
          "worker crash during preload is surfaced and reclaimed");
    Check(ChildProcessIds(GetCurrentProcessId()).empty(),
          "startup failures leave no child worker");
}

void TestAuthFaults() {
    const wchar_t* modes[] = {
        L"mock:disconnect", L"mock:wrong-request", L"mock:bad-embedding",
        L"mock:binding-skip", L"mock:binding-repeat", L"mock:crash-camera",
        L"mock:crash-pad", L"mock:crash-binding3", L"mock:bad-success-timing"
    };
    for (const wchar_t* mode : modes) {
        std::wstring error;
        facelogin::AuthWorkerClient worker(MockConfig(mode), 1000);
        Check(worker.Start(error), "fault-injection worker reaches READY");
        unsigned int bindings = 0;
        const auto result = worker.Authenticate(AcceptCallbacks(bindings));
        Check(!result.succeeded, "fault-injection auth fails closed");
        Check(!worker.IsAlive(), "fault-injection worker is reclaimed");
    }

    std::wstring error;
    facelogin::AuthWorkerClient timeout(MockConfig(L"mock:hang"), 300);
    Check(timeout.Start(error), "timeout worker reaches READY");
    unsigned int bindings = 0;
    const auto timedOut = timeout.Authenticate(AcceptCallbacks(bindings));
    Check(timedOut.timedOut && !timeout.IsAlive(),
          "wedged worker times out and is killed by its Job");

    facelogin::AuthWorkerClient cancelled(MockConfig(L"mock:hang"), 1000);
    error.clear();
    Check(cancelled.Start(error), "cancellation worker reaches READY");
    bindings = 0;
    const auto cancelledResult = cancelled.Authenticate(AcceptCallbacks(bindings, true));
    Check(cancelledResult.cancelled && !cancelled.IsAlive(),
          "parent cancellation kills worker immediately");
    Check(ChildProcessIds(GetCurrentProcessId()).empty(),
          "auth faults leave no child worker");
}

void RunSuccessfulCycle() {
    std::wstring error;
    facelogin::AuthWorkerClient worker(MockConfig(L"mock:success"), 2000);
    Check(worker.Start(error) && worker.IsReady(), "mock worker reaches READY");
    unsigned int bindings = 0;
    const auto result = worker.Authenticate(AcceptCallbacks(bindings));
    Check(result.succeeded && bindings == 3 && result.hasTiming &&
          result.timing.cameraInitMs == 12.5f &&
          result.timing.pipelineMs == 34.5f &&
          result.timing.totalMs == 47.0f,
          "mock auth completes exactly three bindings and returns timing");
    Check(result.cleanupMs == 0.0,
          "deferred-reap success path reports zero cleanup; the caller measures the reap");
    // The success path deliberately returns without reaping (production reaps
    // only after AUTH_SUCCESS delivery so the teardown wait never sits
    // between the match verdict and the Credential Provider). Emulate the
    // caller contract before asserting the child is gone.
    worker.Stop();
    Check(!worker.IsAlive(), "successful one-shot worker exits");
}

void TestRepeatedLifecycle(int cycles) {
    RunSuccessfulCycle();
    RunSuccessfulCycle();
    Metrics baseline;
    Check(ReadMetrics(baseline), "warm parent resource baseline is sampled");

    for (int cycle = 1; cycle <= cycles; ++cycle) {
        RunSuccessfulCycle();
        Check(ChildProcessIds(GetCurrentProcessId()).empty(),
              "completed cycle leaves no child process");
        if (cycle == 1 || cycle % 10 == 0 || cycle == cycles) {
            Metrics current;
            ReadMetrics(current);
            std::printf("cycle=%d handles=%lu threads=%lu private_mib=%.2f working_set_mib=%.2f\n",
                        cycle, current.handles, current.threads,
                        static_cast<double>(current.privateBytes) / (1024.0 * 1024.0),
                        static_cast<double>(current.workingSetBytes) / (1024.0 * 1024.0));
        }
    }

    Metrics finalMetrics;
    Check(ReadMetrics(finalMetrics), "final parent resources are sampled");
    Check(finalMetrics.handles <= baseline.handles + 2,
          "parent handle delta after repeated workers is at most two");
    Check(finalMetrics.threads <= baseline.threads,
          "parent thread count does not grow");
    Check(finalMetrics.privateBytes <= baseline.privateBytes + 10ULL * 1024 * 1024,
          "parent private bytes stay within ten MiB of warm baseline");
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    return length != 0 && length < path.size()
        ? std::wstring(path.data(), length) : std::wstring{};
}

DWORD FindOnlyChild(DWORD parentId) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto children = ChildProcessIds(parentId);
        if (!children.empty()) return children.front();
        Sleep(10);
    }
    return 0;
}

int RunOwnerProbe(const std::wstring& markerPath) {
    std::wstring error;
    facelogin::AuthWorkerClient worker(MockConfig(L"mock:hang"), 1000);
    if (!worker.Start(error)) return 2;
    const DWORD workerId = FindOnlyChild(GetCurrentProcessId());
    if (workerId == 0) return 3;
    {
        std::ofstream marker(markerPath, std::ios::trunc);
        marker << workerId;
    }
    // Deliberately bypass destructors. Closing the process-owned Job handle is
    // what must kill the worker.
    TerminateProcess(GetCurrentProcess(), ERROR_SUCCESS);
    return 4;
}

bool IsProcessGone(DWORD processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, processId);
    if (!process) return true;
    const bool gone = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return gone;
}

void TestOwnerExitKillsJob() {
    wchar_t temp[MAX_PATH] = {};
    Check(GetTempPathW(ARRAYSIZE(temp), temp) != 0,
          "temporary path for owner probe is available");
    const std::wstring marker = std::wstring(temp) +
        L"FaceLogin-AuthWorkerOwner-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    DeleteFileW(marker.c_str());

    const std::wstring executable = CurrentExecutablePath();
    std::wstring command = L"\"" + executable + L"\" --owner-probe \"" + marker + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION owner{};
    const bool created = CreateProcessW(executable.c_str(), mutableCommand.data(),
                                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &owner) != FALSE;
    Check(created, "owner probe process starts");
    if (!created) return;
    CloseHandle(owner.hThread);
    Check(WaitForSingleObject(owner.hProcess, 10000) == WAIT_OBJECT_0,
          "owner probe exits without running destructors");
    CloseHandle(owner.hProcess);

    DWORD workerId = 0;
    {
        std::ifstream markerFile(marker);
        markerFile >> workerId;
    }
    Check(workerId != 0, "owner probe reports its worker pid");
    bool gone = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (IsProcessGone(workerId)) { gone = true; break; }
        Sleep(10);
    }
    Check(gone, "KILL_ON_JOB_CLOSE removes worker when parent exits");
    DeleteFileW(marker.c_str());
}

bool ParseCycles(int argc, wchar_t** argv, int& cycles) {
    if (argc == 1) return true;
    if (argc != 3 || std::wstring(argv[1]) != L"--cycles") return false;
    wchar_t* end = nullptr;
    const long value = std::wcstol(argv[2], &end, 10);
    if (!end || *end != L'\0' || value < 1 || value > 10000) return false;
    cycles = static_cast<int>(value);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    if (argc > 1 && std::wstring(argv[1]) == L"-auth-worker") {
        return ParseWorkerHandles(argc, argv, input, output)
            ? RunMockWorker(input, output) : ERROR_INVALID_PARAMETER;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--owner-probe") {
        return RunOwnerProbe(argv[2]);
    }

    int cycles = 100;
    if (!ParseCycles(argc, argv, cycles)) {
        std::fprintf(stderr, "usage: AuthWorkerLifecycleTest [--cycles N]\n");
        return 2;
    }
    TestStartupFailures();
    TestAuthFaults();
    TestRepeatedLifecycle(cycles);
    TestOwnerExitKillsJob();
    if (g_failures != 0) {
        std::fprintf(stderr, "AuthWorkerLifecycleTest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("AuthWorkerLifecycleTest: all checks passed");
    return 0;
}
