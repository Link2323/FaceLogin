#include "authentication_residency.h"
#include "process_resource_usage.h"
#include "../common/logger.h"

#include <chrono>
#include <exception>
#include <utility>

namespace facelogin {

AuthenticationResidency::~AuthenticationResidency() { Stop(); }

AuthenticationResidency::Use::Use(AuthenticationResidency& owner) : m_owner(&owner) {
    std::lock_guard<std::mutex> lock(owner.m_mutex);
    ++owner.m_activeUsers;
}

AuthenticationResidency::Use::Use(Use&& other) noexcept
    : models(std::move(other.models)), worker(std::move(other.worker)),
      failure(other.failure), error(std::move(other.error)),
      m_owner(std::exchange(other.m_owner, nullptr)) {}

AuthenticationResidency::Use::~Use() {
    models.reset();
    worker.reset();
    if (m_owner) m_owner->EndUse();
}

bool AuthenticationResidency::Start(bool serviceMode, const AppConfig& config, Loaders loaders) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_thread.joinable() || m_stopRequested ||
        (serviceMode ? !loaders.worker : !loaders.models)) {
        FACELOGIN_ERROR(L"Model lifecycle start refused: invalid loader or lifecycle state");
        return false;
    }
    m_serviceMode = serviceMode;
    m_config = config;
    m_loaders = std::move(loaders);
    try {
        m_thread = std::thread(&AuthenticationResidency::Run, this);
        return true;
    } catch (const std::exception& error) {
        FACELOGIN_ERROR(L"Model worker start failed: %hs", error.what());
        return false;
    }
}

void AuthenticationResidency::RequestLoad(const wchar_t* reason) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopRequested) return;
        m_wanted = true;
        if ((m_state == State::Unloaded || m_state == State::Failed) && !m_loadRequested) {
            m_loadRequested = true;
            queued = true;
        }
    }
    m_changed.notify_all();
    if (queued) FACELOGIN_INFO(L"Model load requested: %s", reason);
}

void AuthenticationResidency::RequestUnload(const wchar_t* reason) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopRequested) return;
        queued = m_wanted || m_state != State::Unloaded;
        m_wanted = false;
        m_loadRequested = false;
    }
    m_changed.notify_all();
    if (queued) FACELOGIN_INFO(L"Model unload requested: %s", reason);
}

void AuthenticationResidency::EndUse() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_activeUsers > 0) --m_activeUsers;
    }
    m_changed.notify_all();
}

AuthenticationResidency::Use AuthenticationResidency::Acquire(
    ipc::AuthDeadline deadline, const std::function<void()>& reportWaiting) {
    Use use(*this);
    bool needsWait = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        needsWait = m_loadRequested || m_state != State::Ready ||
            (m_serviceMode && (!m_worker || !m_worker->IsReady()));
        if (!m_stopRequested && m_serviceMode && m_state == State::Ready &&
            (!m_worker || !m_worker->IsReady())) {
            // A one-shot child may exit before the session unlock notification.
            m_state = State::Unloaded;
            m_loadRequested = true;
        }
    }
    if (needsWait && reportWaiting) reportWaiting();
    RequestLoad(m_serviceMode ? L"AUTH_REQUEST worker fallback" : L"AUTH_REQUEST fallback");
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait_until(lock, deadline, [this] {
        // A queued retry/replacement has not produced a verdict yet. The
        // previous Ready/Failed state cannot complete this wait.
        return ((m_state == State::Ready || m_state == State::Failed) && !m_loadRequested) ||
               m_state == State::Stopping || !m_wanted;
    });
    if (m_state == State::Ready && !m_stopRequested && !m_loadRequested) {
        use.models = m_models;
        use.worker = m_worker;
    }
    use.failure = m_failure;
    use.error = m_workerError;
    return use;
}

bool AuthenticationResidency::ReloadConfig(const AppConfig& config) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_config = config;
    uint64_t requiredGeneration = 0;
    bool waitForLoad = false;
    if (m_serviceMode) {
        requiredGeneration = ++m_configGeneration;
        if (m_wanted && m_state != State::Stopping) {
            m_loadRequested = true;
            waitForLoad = true;
        }
    } else if (m_wanted && m_state == State::Failed) {
        m_loadRequested = true;
        waitForLoad = true;
    }
    m_changed.notify_all();
    if (!waitForLoad) return true;
    m_changed.wait(lock, [this, requiredGeneration] {
        return (m_state == State::Ready &&
                (!m_serviceMode || m_loadedGeneration >= requiredGeneration)) ||
               (m_state == State::Failed && !m_loadRequested) ||
               m_state == State::Stopping || !m_wanted;
    });
    return !m_wanted || (m_state == State::Ready &&
        (!m_serviceMode || m_loadedGeneration >= requiredGeneration));
}

void AuthenticationResidency::WorkerConsumed(bool preloadReplacement) {
    std::shared_ptr<AuthWorkerClient> consumed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        consumed = std::move(m_worker);
        if (m_state != State::Stopping) {
            m_state = State::Unloaded;
            if (preloadReplacement && m_wanted && !m_stopRequested) m_loadRequested = true;
        }
    }
    // Job teardown must never hold the state mutex used by session handlers.
    consumed.reset();
    m_changed.notify_all();
}

void AuthenticationResidency::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = true;
        m_wanted = false;
        m_loadRequested = false;
    }
    m_changed.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void AuthenticationResidency::Run() {
    for (;;) {
        std::shared_ptr<InferenceModels> modelsToRelease;
        std::shared_ptr<AuthWorkerClient> workerToRelease;

        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this]() {
            const bool canUnload = !m_wanted &&
                m_state != State::Unloaded &&
                m_state != State::Loading &&
                m_activeUsers == 0;
            return m_stopRequested || m_loadRequested || canUnload;
        });

        if (m_stopRequested) {
            if (m_serviceMode) workerToRelease = std::move(m_worker);
            else modelsToRelease = std::move(m_models);
            m_state = State::Stopping;
            m_changed.notify_all();
            lock.unlock();
            modelsToRelease.reset();
            workerToRelease.reset();
            return;
        }

        if (!m_wanted && m_state != State::Loading &&
            m_activeUsers == 0) {
            if (m_serviceMode) workerToRelease = std::move(m_worker);
            else modelsToRelease = std::move(m_models);
            m_state = State::Unloaded;
            m_failure = ModelLoadFailure::None;
            m_changed.notify_all();
            lock.unlock();
            modelsToRelease.reset();
            workerToRelease.reset();
            const auto usage = CurrentProcessResourceUsage();
            if (m_serviceMode) {
                FACELOGIN_INFO(L"Authentication worker released (unlocked idle state)");
            } else {
                FACELOGIN_INFO(L"Inference models released (unlocked idle state; handles=%lu, private=%.1f MiB, working_set=%.1f MiB)",
                               usage.handles, usage.privateMiB, usage.workingSetMiB);
            }
            continue;
        }

        if (!m_loadRequested || !m_wanted) continue;

        m_loadRequested = false;
        m_state = State::Loading;
        m_changed.notify_all();
        lock.unlock();

        const auto loadStart = std::chrono::steady_clock::now();
        ModelLoadFailure loadFailure = ModelLoadFailure::None;
        std::shared_ptr<InferenceModels> loaded;
        std::shared_ptr<AuthWorkerClient> loadedWorker;
        std::wstring workerError;
        AppConfig workerConfigSnapshot;
        uint64_t workerConfigGeneration = 0;

        // A pending replacement is always one-shot in service mode. Stop the
        // previous child before opening a new one so it cannot retain a camera
        // graph or model heap beside the replacement.
        if (m_serviceMode) {
            lock.lock();
            workerToRelease = std::move(m_worker);
            workerConfigSnapshot = m_config;
            workerConfigGeneration = m_configGeneration;
            lock.unlock();
            workerToRelease.reset();
        }
        try {
            if (m_serviceMode) {
                loadedWorker = m_loaders.worker(workerConfigSnapshot, workerError);
            }
            else loaded = m_loaders.models(loadFailure);
        } catch (const std::exception& e) {
            if (m_serviceMode) {
                FACELOGIN_ERROR(L"Authentication worker launcher threw: %hs", e.what());
            } else {
                FACELOGIN_ERROR(L"Model loader threw: %hs", e.what());
            }
            workerError = L"认证工作进程启动异常";
        }

        bool published = false;
        bool wantedAfterLoad = false;
        lock.lock();
        if (m_stopRequested) {
            m_state = State::Stopping;
        } else if (m_serviceMode && loadedWorker && m_wanted) {
            m_worker = loadedWorker;
            m_state = State::Ready;
            m_failure = ModelLoadFailure::None;
            m_workerError.clear();
            m_loadedGeneration = workerConfigGeneration;
            published = true;
        } else if (!m_serviceMode && loaded && m_wanted) {
            m_models = loaded;
            m_state = State::Ready;
            m_failure = ModelLoadFailure::None;
            published = true;
        } else if (!m_wanted) {
            m_state = State::Unloaded;
            m_failure = ModelLoadFailure::None;
        } else {
            m_state = State::Failed;
            m_failure = loadFailure;
            if (m_serviceMode) {
                m_workerError = workerError.empty()
                    ? L"认证工作进程启动失败，请使用密码登录"
                    : workerError;
            }
        }
        wantedAfterLoad = m_wanted;
        m_changed.notify_all();
        lock.unlock();

        const double loadMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - loadStart).count();
        if (published) {
            const auto usage = CurrentProcessResourceUsage();
            if (m_serviceMode) {
                FACELOGIN_INFO(L"Authentication worker ready in %.0f ms (parent handles=%lu, private=%.1f MiB, working_set=%.1f MiB, threads=%lu)",
                               loadMs, usage.handles, usage.privateMiB,
                               usage.workingSetMiB, usage.threads);
            } else {
                FACELOGIN_INFO(L"Inference models ready in %.0f ms (handles=%lu, private=%.1f MiB, working_set=%.1f MiB)",
                               loadMs, usage.handles, usage.privateMiB, usage.workingSetMiB);
            }
        } else {
            loaded.reset();
            loadedWorker.reset();
            if (m_serviceMode) {
                FACELOGIN_INFO(L"Authentication worker load not published (wanted=%d, %.0f ms)",
                               wantedAfterLoad ? 1 : 0, loadMs);
            } else {
                FACELOGIN_INFO(L"Inference model load not published (wanted=%d, %.0f ms)",
                               wantedAfterLoad ? 1 : 0, loadMs);
            }
        }
    }
}

} // namespace facelogin
