#include "authentication_residency.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <utility>
#include <vector>

// Only this test target replaces worker process ownership. The residency
// implementation and its public interface are the production ones.
namespace facelogin {
AuthWorkerClient::AuthWorkerClient(auth_worker::WorkerConfig config, DWORD timeout,
                                 std::wstring executable)
    : m_config(std::move(config)), m_authSupervisorTimeoutMs(timeout),
      m_executablePathOverride(std::move(executable)), m_ready(true) {}
AuthWorkerClient::~AuthWorkerClient() = default;
bool AuthWorkerClient::IsReady() const { return m_ready; }
void AuthWorkerClient::Stop() { m_ready = false; }
auth_worker::Channel::~Channel() = default;
} // namespace facelogin

namespace {
using namespace facelogin;
using namespace std::chrono_literals;
std::atomic<int> g_failures{0};

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

// A loader can be held at any generation. Bounded waits make a missing state
// transition fail the test instead of leaving the build hung indefinitely.
struct Harness {
    std::mutex mutex;
    std::condition_variable changed;
    int loads = 0;
    int allowed = 100;
    int failLoad = 0;
    std::vector<int> rotations;
    std::map<int, int> releases;
    AuthenticationResidency residency;

    ~Harness() { residency.Stop(); }

    void Start(bool gated = false) {
        allowed = gated ? 0 : 100;
        Check(residency.Start(AppConfig{}, [this](const AppConfig& config, std::wstring& error) {
            std::unique_lock<std::mutex> lock(mutex);
            const int id = ++loads;
            rotations.push_back(config.camera_rotation);
            changed.notify_all();
            const bool released = changed.wait_for(lock, 3s, [&] { return allowed >= id; });
            Check(released, "loader gate released within test deadline");
            if (!released || id == failLoad) {
                error = L"test worker load failed";
                return std::shared_ptr<AuthWorkerClient>{};
            }
            lock.unlock();
            return std::shared_ptr<AuthWorkerClient>(new AuthWorkerClient(auth_worker::WorkerConfig{}), [this, id](auto* worker) {
                delete worker;
                std::lock_guard<std::mutex> releaseLock(mutex);
                ++releases[id];
                changed.notify_all();
            });
        }), "lifecycle starts");
    }

    bool WaitLoads(int count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return loads >= count; });
    }
    bool WaitRelease(int id, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return releases[id] != 0; });
    }
    void Allow(int id) {
        std::lock_guard<std::mutex> lock(mutex);
        allowed = id;
        changed.notify_all();
    }
    AuthenticationResidency::Use Acquire() {
        return residency.Acquire(ipc::MakeAuthDeadline(2000), {});
    }
};

void TestUnlockDuringPreload() {
    Harness h;
    h.Start(true);
    h.residency.RequestLoad(L"test lock");
    Check(h.WaitLoads(1), "preload enters loader");
    h.residency.RequestUnload(L"test unlock during preload");
    h.Allow(1);
    Check(h.WaitRelease(1), "worker loaded after unlock is discarded");
    AppConfig config;
    config.camera_rotation = 90;
    Check(h.residency.ReloadConfig(config), "reload while unlocked succeeds without a worker");
    Check(!h.WaitLoads(2, 30ms), "unlocked reload does not start a worker");
}

void TestUnlockDuringUse() {
    Harness h;
    h.Start();
    {
        auto use = h.Acquire();
        Check(use.worker != nullptr, "authentication acquires worker");
        h.residency.RequestUnload(L"test unlock during authentication");
        // Drop the caller's shared_ptr: protection must come from the Use
        // lifetime, not merely a second reference to the worker object.
        use.worker.reset();
        Check(!h.WaitRelease(1, 30ms), "unlock waits for authentication use to end");
    }
    Check(h.WaitRelease(1), "worker released once use ends");
}

void TestReloadDuringPreload() {
    Harness h;
    h.Start(true);
    h.residency.RequestLoad(L"test lock");
    Check(h.WaitLoads(1), "old configuration is loading");
    AppConfig replacement;
    replacement.camera_rotation = 90;
    std::promise<void> reloadStarted;
    auto reload = std::async(std::launch::async, [&] {
        reloadStarted.set_value();
        return h.residency.ReloadConfig(replacement);
    });
    reloadStarted.get_future().wait();
    Check(reload.wait_for(30ms) != std::future_status::ready,
          "reload waits while original preload is still blocked");
    h.Allow(1);
    Check(h.WaitLoads(2), "reload starts replacement worker");
    Check(reload.wait_for(0ms) != std::future_status::ready,
          "reload cannot acknowledge the older ready generation");
    Check(h.WaitRelease(1), "previous worker reclaimed before replacement is loaded");
    h.Allow(2);
    Check(reload.get(), "reload acknowledges the matching ready generation");
    {
        auto use = h.Acquire();
        Check(use.worker != nullptr, "authentication can use reloaded worker");
    }
    std::lock_guard<std::mutex> lock(h.mutex);
    Check(h.rotations == std::vector<int>({0, 90}), "replacement uses new configuration snapshot");
}

void TestLoadFailureAndRetry() {
    Harness h;
    h.failLoad = 1;
    h.Start();
    {
        auto failed = h.Acquire();
        Check(!failed.worker && failed.error == L"test worker load failed",
              "load failure returns its error without a worker");
    }
    {
        auto retry = h.Acquire();
        Check(retry.worker != nullptr && retry.error.empty(),
              "next request retries failed preload and clears old error");
    }
}

void TestReloadFailure() {
    Harness h;
    h.failLoad = 2;
    h.Start();
    {
        auto original = h.Acquire();
        Check(original.worker != nullptr, "original worker ready before failed reload");
    }
    AppConfig replacement;
    replacement.camera_rotation = 180;
    Check(!h.residency.ReloadConfig(replacement), "failed replacement rejects config reload acknowledgement");
    Check(h.WaitRelease(1), "failed reload cannot retain the old ready worker");
    {
        auto retry = h.Acquire();
        Check(retry.worker != nullptr && retry.error.empty(), "authentication retries failed replacement");
    }
    std::lock_guard<std::mutex> lock(h.mutex);
    Check(h.rotations == std::vector<int>({0, 180, 180}), "retry retains the replacement configuration");
}

void TestUnlockDuringReload() {
    Harness h;
    h.Start(true);
    h.residency.RequestLoad(L"test lock");
    Check(h.WaitLoads(1), "initial preload starts");
    h.Allow(1);
    {
        auto use = h.Acquire();
        Check(use.worker != nullptr, "initial preload ready before unlock/reload");
    }
    auto reload = std::async(std::launch::async, [&] { return h.residency.ReloadConfig(AppConfig{}); });
    Check(h.WaitLoads(2), "config replacement is loading");
    h.residency.RequestUnload(L"test unlock during reload");
    Check(reload.get(), "intentional unload releases reload waiter without requiring a resident worker");
    h.Allow(2);
    Check(h.WaitRelease(2), "replacement finishing after unlock is discarded");
}

void TestExpiredAcquireDoesNotRestartBudget() {
    Harness h;
    h.Start(true);
    h.residency.RequestLoad(L"test lock");
    Check(h.WaitLoads(1), "worker held while caller budget expires");
    {
        const auto start = std::chrono::steady_clock::now();
        auto use = h.residency.Acquire(ipc::MakeAuthDeadline(0), {});
        Check(!use.worker, "expired caller does not wait for loader or acquire incomplete worker");
        Check(std::chrono::steady_clock::now() - start < 200ms,
              "acquisition cannot restart an expired authentication budget");
    }
    h.Allow(1);
    {
        auto next = h.Acquire();
        Check(next.worker != nullptr, "later request can acquire the completed preload");
    }
}

void TestExitedWorkerReplacement() {
    Harness h;
    h.Start();
    {
        auto first = h.Acquire();
        Check(first.worker != nullptr, "initial worker ready");
        if (first.worker) first.worker->Stop(); // child exited before next request
    }
    {
        auto replacement = h.Acquire();
        Check(replacement.worker && replacement.worker->IsReady(),
              "request replaces worker that exited while marked ready");
    }
    Check(h.WaitRelease(1), "exited worker is reclaimed");
    Check(h.WaitLoads(2), "replacement was actually loaded");
}

void TestConsumedWorkerPolicy(bool failure) {
    Harness h;
    h.Start();
    {
        auto use = h.Acquire();
        Check(use.worker != nullptr, "worker ready before terminal result");
    }
    h.residency.WorkerConsumed(failure);
    Check(h.WaitRelease(1), "terminal result releases consumed worker");
    if (failure) {
        Check(h.WaitLoads(2), "failure preloads a replacement while locked");
    } else {
        Check(!h.WaitLoads(2, 30ms), "successful handoff does not preload a second worker");
    }
    {
        auto next = h.Acquire();
        Check(next.worker != nullptr, "next request obtains a fresh worker");
    }
    std::lock_guard<std::mutex> lock(h.mutex);
    Check(h.loads == 2 && h.releases[1] == 1, "consumption yields exactly one replacement and one release");
}

void TestStopDuringUse() {
    Harness h;
    h.Start();
    {
        auto use = h.Acquire();
        Check(use.worker != nullptr, "worker acquired before service stop");
        h.residency.Stop();
        Check(!h.WaitRelease(1, 30ms), "stop preserves worker still held by authentication");
    }
    Check(h.WaitRelease(1), "last authentication reference releases stopped worker");
    h.residency.RequestLoad(L"ignored after stop");
    {
        auto stopped = h.Acquire();
        Check(!stopped.worker, "stopped residency cannot supply or reload worker");
    }
    Check(!h.WaitLoads(2, 30ms), "stop prevents later load requests");
}

void TestStopDuringPreload() {
    Harness h;
    // Keep Start/Stop on the same owner thread. The test thread only controls
    // the loader gate, as model/process initialization would finish separately.
    auto owner = std::async(std::launch::async, [&] {
        h.Start(true);
        h.residency.RequestLoad(L"test preload before stop");
        Check(h.WaitLoads(1), "preload entered before stop");
        h.residency.Stop();
    });
    Check(h.WaitLoads(1), "test observes blocked preload");
    Check(owner.wait_for(30ms) != std::future_status::ready,
          "stop joins pending loader before releasing dependencies");
    h.Allow(1);
    owner.get();
    Check(h.WaitRelease(1), "worker finishing after stop is discarded");
    auto use = h.Acquire();
    Check(!use.worker, "stopped loader result is never published for authentication");
}
} // namespace

int main() {
    TestUnlockDuringPreload();
    TestUnlockDuringUse();
    TestReloadDuringPreload();
    TestLoadFailureAndRetry();
    TestReloadFailure();
    TestUnlockDuringReload();
    TestExpiredAcquireDoesNotRestartBudget();
    TestExitedWorkerReplacement();
    TestConsumedWorkerPolicy(false);
    TestConsumedWorkerPolicy(true);
    TestStopDuringUse();
    TestStopDuringPreload();
    std::printf("AuthenticationResidencyTest: %s (%d failures)\n",
                g_failures == 0 ? "passed" : "FAILED", g_failures.load());
    return g_failures == 0 ? 0 : 1;
}
