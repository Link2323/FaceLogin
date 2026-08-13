// Dev-only diagnostic for the "解锁后 ORT/Windows 堆高水位" issue
// (docs/todo.md). Never shipped.
//
// Mirrors the production model lifecycle (FaceService::LoadInferenceModels:
// the same 4-model bundle — SCRFD det + w600k_r50 + dual MiniFAS — sharing the
// same process-lifetime ONNX Env) WITHOUT camera / pipe / service-thread
// noise, then destroys the bundle and samples process resources each round.
// The point is to see whether ORT returns to a low private-bytes water mark
// after session destruction, and whether the residual is a single step
// (one-time expansion) or keeps growing with each load→infer→destroy cycle.
//
// Usage: OrtMemoryTest <models_dir> [rounds] [settle_ms]
//   models_dir  dir holding det_10g_gnkps.onnx, w600k_r50.onnx,
//               MiniFASNetV2.onnx, MiniFASNetV1SE.onnx
//   rounds      load→infer→destroy cycles (default 100)
//   settle_ms   sleep after bundle destruction before sampling (default 200)
//
// Output: one CSV row per round:
//   round,load_infer_ms,private_mib,wss_mib,commit_priv_mib,
//   largest_region_mib,heap_inuse_mib,handles,threads
// The load_infer_ms column lets the arena A/B also compare per-round auth
// cost (the model load + one inference each is most of an auth's CPU cost).
// commit_priv_mib / largest_region_mib / heap_inuse_mib pin down the source
// of the residual private bytes: committed-but-free heap segment space vs
// live allocations (see SampleProcess).
#include "onnx_models.h"
#include "../common/frame_image.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

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

std::wstring Utf8ToWstr(const char* text) {
    if (!text || !*text) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], n);
    return out;
}

DWORD ProcessThreadCount() {
    DWORD pid = GetCurrentProcessId();
    DWORD count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) ++count;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

struct Sample {
    double privateMiB = 0.0;
    double wssMiB = 0.0;
    double committedPrivateMiB = 0.0;  // sum of MEM_COMMIT|MEM_PRIVATE regions
    double largestRegionMiB = 0.0;     // single largest committed private region
    double heapInUseMiB = 0.0;         // live bytes in the default process heap
    DWORD handles = 0;
    DWORD threads = 0;
};

Sample SampleProcess() {
    Sample s;
    GetProcessHandleCount(GetCurrentProcess(), &s.handles);
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        s.privateMiB = static_cast<double>(counters.PrivateUsage) / kMiB;
        s.wssMiB = static_cast<double>(counters.WorkingSetSize) / kMiB;
    }

    // Enumerate the virtual address space: total committed private bytes and
    // the single largest committed private region. A ~32 MiB heap segment the
    // Windows heap has committed but not returned shows up here as one large
    // MEM_PRIVATE|MEM_COMMIT region whose size stays roughly constant while
    // the "live" heap bytes (HeapWalk below) barely change — the signature of
    // a committed heap high-water that no in-process API can return.
    MEMORY_BASIC_INFORMATION mbi = {};
    for (unsigned char* p = nullptr;
         VirtualQuery(p, &mbi, sizeof(mbi)) != 0;
         p += mbi.RegionSize) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
            s.committedPrivateMiB += static_cast<double>(mbi.RegionSize) / kMiB;
            if (mbi.RegionSize > s.largestRegionMiB) {
                s.largestRegionMiB = static_cast<double>(mbi.RegionSize) / kMiB;
            }
        }
    }

    // Live (BUSY) allocation bytes in the default process heap — the space
    // actually owned by malloc/ORT right now, as opposed to committed-but-free
    // segment space.
    PROCESS_HEAP_ENTRY entry = {};
    size_t inUse = 0;
    HANDLE heap = GetProcessHeap();
    while (HeapWalk(heap, &entry)) {
        if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) inUse += entry.cbData;
    }
    s.heapInUseMiB = static_cast<double>(inUse) / kMiB;

    s.threads = ProcessThreadCount();
    return s;
}

// A neutral 640×480 frame so every model's graph actually runs (the allocator
// behavior under test is what matters, not the recognition result).
facelogin::FrameImage BlankFrame() {
    facelogin::FrameImage frame(480, 640);
    for (long i = 0; i < frame.size(); ++i) {
        auto& p = frame(i);
        p.red = p.green = p.blue = 128;
    }
    return frame;
}

} // namespace

int main(int argc, char** argv) {
    int rounds = 100;
    int settleMs = 200;
    if (argc < 2 || argc > 4 ||
        (argc >= 3 && !ParsePositiveInt(argv[2], rounds)) ||
        (argc == 4 && !ParsePositiveInt(argv[3], settleMs))) {
        std::cerr << "usage: OrtMemoryTest <models_dir> [rounds] [settle_ms]\n";
        return 2;
    }

    const std::wstring modelsDir = Utf8ToWstr(argv[1]);
    if (modelsDir.empty()) {
        std::cerr << "models_dir is empty\n";
        return 2;
    }
    const std::wstring detPath = modelsDir + L"\\det_10g_gnkps.onnx";
    const std::wstring recPath = modelsDir + L"\\w600k_r50.onnx";
    const std::wstring v2Path = modelsDir + L"\\MiniFASNetV2.onnx";
    const std::wstring v1sePath = modelsDir + L"\\MiniFASNetV1SE.onnx";

    // Force the process-lifetime Envs to exist once (as in production) so
    // round 1 captures their cost, not a later-round surprise.
    const Sample pre = SampleProcess();
    std::cout << "round,load_infer_ms,private_mib,wss_mib,commit_priv_mib,"
                 "largest_region_mib,heap_inuse_mib,handles,threads\n";
    std::cout << "pre,0," << pre.privateMiB << "," << pre.wssMiB << ","
              << pre.committedPrivateMiB << "," << pre.largestRegionMiB << ","
              << pre.heapInUseMiB << "," << pre.handles << "," << pre.threads
              << "\n";

    const facelogin::FrameImage frame = BlankFrame();
    facelogin::FrameImage chip(64, 64);
    for (long i = 0; i < chip.size(); ++i) {
        auto& p = chip(i);
        p.red = p.green = p.blue = 128;
    }
    const facelogin::FaceRect rect(200, 150, 440, 330);

    for (int round = 1; round <= rounds; ++round) {
        const auto roundStart = std::chrono::steady_clock::now();
        try {
            // === Load the 4-model bundle (same classes as production) ===
            facelogin::OnnxDetector detector;
            facelogin::OnnxRecognizer recognizer;
            facelogin::OnnxAntiSpoof antiSpoof;
            if (!detector.Initialize(detPath) ||
                !recognizer.Initialize(recPath) ||
                !antiSpoof.Initialize(v2Path, v1sePath)) {
                std::cerr << "model init failed at round " << round << "\n";
                return 1;
            }

            // === Run one inference per model to exercise the allocator ===
            (void)detector.Detect(frame);
            (void)recognizer.ComputeEmbedding(chip);
            (void)antiSpoof.Predict(frame, rect);
            // Bundle destroyed here (scope exit) — mirrors the service
            // releasing InferenceModels on unlock.
        } catch (const std::exception& e) {
            std::cerr << "exception at round " << round << ": " << e.what()
                      << "\n";
            return 1;
        }
        const double loadInferMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - roundStart)
                .count();

        Sleep(settleMs);  // let the heap settle before sampling
        const Sample s = SampleProcess();
        std::cout << round << "," << loadInferMs << "," << s.privateMiB << ","
                  << s.wssMiB << "," << s.committedPrivateMiB << ","
                  << s.largestRegionMiB << "," << s.heapInUseMiB << ","
                  << s.handles << "," << s.threads << "\n";
    }
    return 0;
}
