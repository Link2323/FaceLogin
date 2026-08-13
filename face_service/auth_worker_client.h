#pragma once

#include "auth_pipeline.h"
#include "auth_worker_protocol.h"

#include <windows.h>

#include <functional>
#include <string>

namespace facelogin {

struct AuthWorkerCallbacks {
    std::function<void(const std::wstring&)> reportStatus;
    std::function<bool()> isCancelled;
    std::function<BindingDecision(const std::vector<float>&,
                                  unsigned int bindingIndex)> verifyBinding;
};

struct AuthWorkerResult {
    bool succeeded = false;
    bool cancelled = false;
    bool timedOut = false;
    bool hasTiming = false;
    auth_worker::AuthTiming timing;
    double supervisorElapsedMs = 0.0;
    double cleanupMs = 0.0;
    std::wstring errorMessage;
};

// Parent-side owner for one private, one-authentication worker. It is safe to
// construct/load on the FaceService lifecycle thread and use Authenticate from
// its public-pipe thread; no other thread may use it concurrently.
class AuthWorkerClient {
public:
    explicit AuthWorkerClient(auth_worker::WorkerConfig config,
                              DWORD authSupervisorTimeoutMs = 20000,
                              std::wstring executablePathOverride = {});
    ~AuthWorkerClient();
    AuthWorkerClient(const AuthWorkerClient&) = delete;
    AuthWorkerClient& operator=(const AuthWorkerClient&) = delete;

    bool Start(std::wstring& errorMessage);
    AuthWorkerResult Authenticate(AuthWorkerCallbacks callbacks);
    void Stop();
    bool IsReady() const;
    bool IsAlive() const;

private:
    bool Spawn(std::wstring& errorMessage);
    bool ReadStartupMessage(auth_worker::MessageType expected,
                            DWORD timeoutMs, std::wstring& errorMessage);
    // A validated terminal message commits the authentication result, but the
    // worker Job must be fully reclaimed before credentials are released.
    // Reclaim the one-shot Job before returning terminal ownership to the
    // caller, keeping camera/ORT resources outside the persistent parent.
    void CompleteTerminal();
    void CloseProcessHandles();

    auth_worker::WorkerConfig m_config;
    auth_worker::Channel m_channel;
    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr;
    uint64_t m_nextRequestId = 1;
    DWORD m_authSupervisorTimeoutMs = 20000;
    std::wstring m_executablePathOverride;
    bool m_ready = false;
};

} // namespace facelogin
