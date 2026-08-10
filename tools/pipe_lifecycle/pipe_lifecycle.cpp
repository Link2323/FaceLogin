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
// Pass criteria (review item #9, P2):
//   1. After every Close(), PipeServer::OutstandingAclAllocations() == 0.
//   2. Private bytes at the final cycle vs the warmup cycle grow by <= 1 MiB
//      (headroom for heap fragmentation; the fix itself leaks nothing).
//
// Uses a distinct test pipe name, so it runs even while the FaceLogin service
// is up (the service's single instance of ipc::PIPE_NAME would otherwise make
// CreateNamedPipeW fail with ERROR_PIPE_BUSY).
#include "pipe_server.h"

#include <cstdlib>
#include <iostream>
#include <limits>

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

} // namespace

int main(int argc, char** argv) {
    int iterations = 1000;
    int warmup = 100;
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
