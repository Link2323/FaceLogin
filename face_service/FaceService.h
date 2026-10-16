#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>

#include "../vision/liveness_types.h"
#include "authentication_residency.h"
#include "template_learner.h"
#include "pipe_server.h"
#include "../storage/credential_store.h"
#include "../common/config_util.h"

namespace facelogin {

// Windows service implementing the face recognition pipeline.
// Runs as LOCAL SYSTEM (required for DPAPI machine-scope decryption
// and credential provider IPC).
//
// Lifecycle:
//   1. ServiceMain called by SCM
//   2. HandlerEx handles start/stop/pause
//   3. Run() is the main loop: accept pipe connections, process auth requests
//
// The face recognition pipeline:
//   Webcam -> SCRFD detection (+5 keypoints) -> 5-point similarity alignment
//   -> 128-D SFace embedding -> Match against stored DB -> dual MiniFAS silent
//   anti-spoof check -> Send credentials

class FaceService {
public:
    FaceService();
    ~FaceService();

    // Service entry points
    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI HandlerEx(DWORD control, DWORD eventType,
                                   LPVOID eventData, LPVOID context);

    // Install/uninstall the service
    static bool Install(const std::wstring& exePath);
    static bool Uninstall();

private:
    void Run();          // Main service loop
    void Stop();         // Heavy cleanup; called by the service main thread
    void RequestStop();  // Non-blocking SCM control-handler path
    bool Initialize();   // Load DB/config and start the model lifecycle worker
    bool ProcessAuthRequest();  // Handle one auth session

    // The parent binds the first worker embedding to a SID; later binding
    // frames must match that SID or the round is rejected as a face swap.
    // identityMissDistance (optional) collects the closest identity distance
    // seen on Retry verdicts within one auth round, for the failure log —
    // distinguishing "just above threshold" from "way off" post-mortem.
    // roundBest (optional) accumulates the best-distance accepted binding
    // frame of the round as a progressive-learning candidate (best wins);
    // yawDeg/pitchDeg ride along as the candidate's pose for the cone gate.
    BindingDecision VerifyIdentityBinding(
        const std::vector<float>& embedding, unsigned int bindingIndex,
        float preNorm, float yawDeg, float pitchDeg,
        std::optional<CredentialStore::IdentityMatch>& lockedIdentity,
        std::wstring& initialSid,
        float* identityMissDistance = nullptr,
        LearningSample* roundBest = nullptr) const;

    // Persist template-learning updates. Runs only on the learner thread or
    // service shutdown: takes the store lock, writes the daily users.dat.bak
    // on the first flush of a local day, then SaveDatabase().
    void FlushLearnedTemplates();

    // Record the round's distance in the user's rolling window, compute the
    // era p20 for the adaptive update gate, and hand the sample to the
    // learner. Public-pipe thread only (called after
    // AUTH_SUCCESS delivery); RELOAD_DB clears the windows (era reset).
    void SubmitLearningCandidate(const LearningSample& best);

    // LEARNING_STATUS response body: learner snapshot (events + per-face
    // counters) merged with the era windows and the active learning config.
    // Single UTF-16 JSON message, no fragmentation — keep the payload within
    // the client's read buffer. Public-pipe thread only.
    std::wstring BuildLearningStatusJson();

    // Public-pipe message helpers. STATUS is advisory and needs no ACK;
    // terminal/control responses use explicit bounded acknowledgements before
    // disconnect so no path depends on unbounded FlushFileBuffers.
    bool SendStatusMessage(const std::wstring& text);
    bool SendTerminalMessage(const std::wstring& message);
    bool SendControlResponse(const std::wstring& message);
    bool SendAcknowledgedMessage(const std::wstring& message,
                                 const wchar_t* expectedAck);
    bool SendAuthErrorMessage(const std::wstring& message);

    std::shared_ptr<AuthWorkerClient> LoadAuthenticationWorker(const AppConfig& config,
                                                              std::wstring& errorMessage);

    // Service state
    SERVICE_STATUS_HANDLE m_hStatus = nullptr;
    SERVICE_STATUS m_status = {};
    std::atomic<bool> m_running{false};
    static FaceService* s_pInstance;

    // Components
    std::unique_ptr<PipeServer> m_pipeServer;
    std::unique_ptr<CredentialStore> m_store;
    // Serializes every m_store access. Historically single-threaded (public
    // pipe thread only); template learning added a second access path (the
    // learner thread), so reads and writes now share this lock. Mutable:
    // VerifyIdentityBinding takes it from a const method.
    mutable CRITICAL_SECTION m_storeLock{};
    bool m_storeLockReady = false;
    std::unique_ptr<TemplateLearner> m_learner;
    int m_learningBakDay = 0;   // local yyyymmdd of the last users.dat.bak

    // Configuration
    AppConfig m_config;
    float m_antiSpoofThreshold = 0.28f;

    // Settings
    std::wstring m_dataDir;
    std::wstring m_modelsDir;
    float m_matchThreshold = 0.30f;
    int m_authTimeoutSeconds = ipc::AUTH_TIMEOUT_SECONDS;

    AuthenticationResidency m_residency;

};

} // namespace facelogin
