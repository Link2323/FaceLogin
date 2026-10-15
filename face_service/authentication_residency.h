#pragma once

#include "auth_worker_client.h"
#include "../common/config_util.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace facelogin {

// Serializes preload, configuration replacement and idle release. Session
// handlers only queue requests; loaders and resource destruction run outside
// the state mutex. The owner calls Stop before its loader dependencies expire,
// and all Use objects must expire before the residency object is destroyed.
// Start/Stop run on the owner thread; Acquire, ReloadConfig and WorkerConsumed
// are serialized by the public-pipe thread. Session request methods may be
// called concurrently from the SCM control handler.
class AuthenticationResidency {
public:
    using WorkerLoader = std::function<std::shared_ptr<AuthWorkerClient>(
        const AppConfig&, std::wstring&)>;

    class Use {
    public:
        ~Use();
        Use(Use&& other) noexcept;
        Use(const Use&) = delete;
        Use& operator=(const Use&) = delete;
        Use& operator=(Use&&) = delete;

        std::shared_ptr<AuthWorkerClient> worker;
        std::wstring error;

    private:
        friend class AuthenticationResidency;
        explicit Use(AuthenticationResidency& owner);
        AuthenticationResidency* m_owner;
    };

    ~AuthenticationResidency();
    bool Start(const AppConfig& config, WorkerLoader loader);
    void RequestLoad(const wchar_t* reason);
    void RequestUnload(const wchar_t* reason);
    void Stop();

    // Uses the caller's original deadline. The use count protects resources
    // during preparation and authentication, even after an unlock notification.
    Use Acquire(ipc::AuthDeadline deadline, const std::function<void()>& reportWaiting);
    // Replaces a locked service worker and waits for the matching generation.
    // An intentionally unloaded desktop stays unloaded.
    bool ReloadConfig(const AppConfig& config);
    // Called after the parent decides the complete public result. Successful
    // handoff uses false; failure may request a replacement while still locked.
    void WorkerConsumed(bool preloadReplacement);

private:
    enum class State { Unloaded, Loading, Ready, Failed, Stopping };
    void Run();
    void EndUse();

    AppConfig m_config;
    WorkerLoader m_loader;
    std::shared_ptr<AuthWorkerClient> m_worker;
    State m_state = State::Unloaded;
    bool m_wanted = false;
    bool m_loadRequested = false;
    bool m_stopRequested = false;
    unsigned int m_activeUsers = 0;
    uint64_t m_configGeneration = 0;
    uint64_t m_loadedGeneration = 0;
    std::wstring m_workerError;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_changed;
};

} // namespace facelogin
