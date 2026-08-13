// Production DirectShow camera lifecycle probe.
//
// Examples:
//   CameraLifecycleTest --mode child --cycles 100 --settle-ms 500
#include "webcam_capture_dshow.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
enum class RunMode { InProcess, Child };

struct Options {
    RunMode mode = RunMode::InProcess;
    int cycles = 10;
    int settleMs = 500;
    int singleCycleNumber = 0;
};

struct ProcessMetrics {
    DWORD handles = 0;
    DWORD threads = 0;
    SIZE_T privateBytes = 0;
    SIZE_T workingSetBytes = 0;
};

bool ParseInt(const wchar_t* text, int minimum, int maximum, int& value) {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (!text || !text[0] || !end || *end != L'\0' ||
        parsed < minimum || parsed > maximum) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool ParseOptions(int argc, wchar_t** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--mode" && i + 1 < argc) {
            const std::wstring value = argv[++i];
            if (value == L"inproc") options.mode = RunMode::InProcess;
            else if (value == L"child") options.mode = RunMode::Child;
            else return false;
        } else if (arg == L"--cycles" && i + 1 < argc) {
            if (!ParseInt(argv[++i], 1, 10000, options.cycles)) return false;
        } else if (arg == L"--settle-ms" && i + 1 < argc) {
            if (!ParseInt(argv[++i], 0, 60000, options.settleMs)) return false;
        } else if (arg == L"--single-cycle" && i + 1 < argc) {
            if (!ParseInt(argv[++i], 1, 10000, options.singleCycleNumber)) return false;
            options.mode = RunMode::InProcess;
            options.cycles = 1;
            options.settleMs = 0;
        } else {
            return false;
        }
    }
    return true;
}

DWORD CountCurrentProcessThreads() {
    const DWORD processId = GetCurrentProcessId();
    DWORD count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
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

bool ReadProcessMetrics(ProcessMetrics& metrics) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessHandleCount(GetCurrentProcess(), &metrics.handles) ||
        !GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) return false;
    metrics.threads = CountCurrentProcessThreads();
    metrics.privateBytes = counters.PrivateUsage;
    metrics.workingSetBytes = counters.WorkingSetSize;
    return true;
}

double Milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void PrintMetrics(const char* record, int cycle, const ProcessMetrics& metrics) {
    constexpr double kMiB = 1024.0 * 1024.0;
    std::cout << record << " cycle=" << cycle
              << " handles=" << metrics.handles
              << " threads=" << metrics.threads
              << " private_mib=" << std::fixed << std::setprecision(2)
              << static_cast<double>(metrics.privateBytes) / kMiB
              << " working_set_mib="
              << static_cast<double>(metrics.workingSetBytes) / kMiB << "\n";
}

std::vector<facelogin::CameraDeviceInfo> Enumerate() {
    return facelogin::WebcamCaptureDS::ListCameras();
}

bool RunOneCameraCycle(const Options&, int cycle) {
    const auto enumStart = Clock::now();
    const auto devices = Enumerate();
    const auto enumDone = Clock::now();
    if (devices.empty()) {
        std::cerr << "camera enumeration returned no devices at cycle " << cycle << "\n";
        return false;
    }

    auto camera = std::make_unique<facelogin::WebcamCaptureDS>();
    const auto activateStart = Clock::now();
    if (!camera->Initialize(640, 480, L"")) {
        std::cerr << "camera initialization failed at cycle " << cycle << "\n";
        return false;
    }
    const auto activateDone = Clock::now();

    int validFrames = 0;
    double firstFrameMs = -1.0;
    const auto frameDeadline = Clock::now() + std::chrono::seconds(8);
    while (validFrames < 10 && Clock::now() < frameDeadline) {
        facelogin::FrameImage frame;
        if (camera->GrabFrame(frame) && frame.nc() == 640 && frame.nr() == 480) {
            if (validFrames == 0) firstFrameMs = Milliseconds(activateDone, Clock::now());
            ++validFrames;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    const auto shutdownStart = Clock::now();
    camera->Pause();
    camera->Shutdown();
    camera.reset();
    const auto shutdownDone = Clock::now();

    ProcessMetrics metrics;
    if (!ReadProcessMetrics(metrics)) return false;
    std::cout << "camera_cycle cycle=" << cycle
              << " backend=dshow"
              << " enum_ms=" << std::fixed << std::setprecision(1)
              << Milliseconds(enumStart, enumDone)
              << " activate_ms=" << Milliseconds(activateStart, activateDone)
              << " first_frame_ms=" << firstFrameMs
              << " shutdown_ms=" << Milliseconds(shutdownStart, shutdownDone)
              << " frames=" << validFrames
              << " devices=" << devices.size()
              << " handles=" << metrics.handles
              << " threads=" << metrics.threads
              << " private_mib=" << std::setprecision(2)
              << static_cast<double>(metrics.privateBytes) / (1024.0 * 1024.0)
              << " working_set_mib="
              << static_cast<double>(metrics.workingSetBytes) / (1024.0 * 1024.0)
              << "\n";
    return validFrames >= 10;
}

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::wstring(path.data(), length);
}

bool RunChildCycle(const Options&, int cycle) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) return false;
    std::wstring command = L"\"" + executable + L"\" --single-cycle " +
        std::to_wstring(cycle);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr,
                        TRUE, CREATE_SUSPENDED, nullptr, nullptr,
                        &startup, &process)) {
        std::cerr << "failed to create child cycle " << cycle
                  << " error=" << GetLastError() << "\n";
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, ERROR_ACCESS_DENIED);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (job) CloseHandle(job);
        return false;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) TerminateJobObject(job, ERROR_TIMEOUT);
    DWORD exitCode = ERROR_TIMEOUT;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    return wait == WAIT_OBJECT_0 && exitCode == ERROR_SUCCESS;
}

void Usage() {
    std::cerr << "usage: CameraLifecycleTest --mode inproc|child --cycles N --settle-ms N\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        Usage();
        return 2;
    }

    ProcessMetrics initial;
    if (!ReadProcessMetrics(initial)) return 1;
    PrintMetrics("baseline", 0, initial);

    for (int offset = 0; offset < options.cycles; ++offset) {
        const int cycle = options.singleCycleNumber != 0
            ? options.singleCycleNumber : offset + 1;
        const bool ok = options.mode == RunMode::InProcess
            ? RunOneCameraCycle(options, cycle)
            : RunChildCycle(options, cycle);
        if (!ok) return 1;

        if (options.settleMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.settleMs));
        }
        ProcessMetrics settled;
        if (!ReadProcessMetrics(settled)) return 1;
        PrintMetrics(options.mode == RunMode::Child ? "parent_settled" : "settled",
                     cycle, settled);
    }
    return 0;
}
