// Repeatedly constructs and destroys the production DirectShow graph, then
// samples process resources after teardown. This isolates camera-driver/filter
// lifetime from ONNX model and authentication lifetime.
//
// Usage: CameraLifecycleTest [cycles] [settle_ms]
#include "webcam_capture_dshow.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include <windows.h>
#include <psapi.h>

namespace {

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

bool PrintProcessMetrics(int cycle) {
    DWORD handles = 0;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles) ||
        !GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        std::cerr << "process metrics unavailable (error=" << GetLastError()
                  << ")\n";
        return false;
    }

    constexpr double kMiB = 1024.0 * 1024.0;
    std::cout << "cycle=" << cycle
              << " handles=" << handles
              << " private_mib="
              << static_cast<double>(counters.PrivateUsage) / kMiB
              << " working_set_mib="
              << static_cast<double>(counters.WorkingSetSize) / kMiB
              << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int cycles = 10;
    int settleMs = 500;
    if (argc > 3 ||
        (argc >= 2 && !ParsePositiveInt(argv[1], cycles)) ||
        (argc == 3 && !ParsePositiveInt(argv[2], settleMs))) {
        std::cerr << "usage: CameraLifecycleTest [cycles] [settle_ms]\n";
        return 2;
    }

    if (!PrintProcessMetrics(0)) return 1;

    for (int cycle = 1; cycle <= cycles; ++cycle) {
        {
            facelogin::WebcamCaptureDS camera;
            if (!camera.Initialize(640, 480)) {
                std::cerr << "camera initialization failed at cycle " << cycle
                          << "\n";
                return 1;
            }

            // Exercise the running graph and callback, not just COM creation.
            facelogin::FrameImage frame;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
            while (!camera.GrabFrame(frame) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            camera.Shutdown();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
        if (!PrintProcessMetrics(cycle)) return 1;
    }
    return 0;
}
