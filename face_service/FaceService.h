#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "liveness_types.h"
#include "model_failure.h"
#include "onnx_models.h"
#include "auth_worker_client.h"
#include "template_learner.h"
#include "webcam_capture_dshow.h"
#include "pipe_server.h"
#include "credential_store.h"
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
//   -> 512-D embedding -> Match against stored DB -> dual MiniFAS silent
//   anti-spoof check -> Send credentials

class FaceService {
public:
    FaceService();
    ~FaceService();

    // Service entry points
    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI HandlerEx(DWORD control, DWORD eventType,
                                   LPVOID eventData, LPVOID context);

    // Run standalone (foreground, for testing) — no SCM registration
    static void RunStandalone();

    // Install/uninstall the service
    static bool Install(const std::wstring& exePath);
    static bool Uninstall();

private:
    struct InferenceModels {
        std::unique_ptr<OnnxDetector> detector;
        std::unique_ptr<OnnxRecognizer> recognizer;
        std::unique_ptr<OnnxAntiSpoof> antiSpoof;
    };

    enum class ModelState {
        Unloaded,
        Loading,
        Ready,
        Failed,
        Stopping,
    };

    class ModelUseGuard {
    public:
        explicit ModelUseGuard(FaceService& service) : m_service(service) {
            m_service.BeginModelUse();
        }
        ~ModelUseGuard() { m_service.EndModelUse(); }
        ModelUseGuard(const ModelUseGuard&) = delete;
        ModelUseGuard& operator=(const ModelUseGuard&) = delete;
    private:
        FaceService& m_service;
    };

    void Run();          // Main service loop
    void Stop();         // Heavy cleanup; called by the service main thread
    void RequestStop();  // Non-blocking SCM control-handler path
    bool Initialize();   // Load DB/config and start the model lifecycle worker
    bool ProcessAuthRequest();  // Handle one auth session
    bool ProcessWorkerAuthRequest(); // Service-mode auth via child process

    // Shared identity-binding rule for both auth hosts (in-process pipeline
    // and worker child): the first anchor locks the matched SID, later
    // binding frames must return the same SID or the round is rejected as a
    // face swap. Single source — the security rule must not exist as two
    // copies that can drift.
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
    // learner. Public-pipe thread only (both auth paths call it after
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

    // Model residency follows the interactive session: preload all inference
    // sessions when Windows locks, retain them across failed auth retries, and
    // release them after unlock. AUTH_REQUEST always requests + waits as a
    // fallback in case a session notification was missed. A long-lived worker
    // owns construction/destruction so HandlerEx never performs heavy work.
    bool StartModelWorker();
    void ModelWorkerLoop();
    void StopModelWorker();
    void RequestModelLoad(const wchar_t* reason);
    void RequestModelUnload(const wchar_t* reason);
    std::shared_ptr<InferenceModels> AcquireModelsForAuth();
    std::shared_ptr<InferenceModels> LoadInferenceModels(ModelLoadFailure& failureReason);
    std::shared_ptr<AuthWorkerClient> AcquireAuthWorkerForAuth();
    std::shared_ptr<AuthWorkerClient> LoadAuthenticationWorker(const AppConfig& config,
                                                                std::wstring& errorMessage);
    void MarkAuthWorkerConsumed(bool preloadReplacement);
    void BeginModelUse();
    void EndModelUse();

    // Service state
    SERVICE_STATUS_HANDLE m_hStatus = nullptr;
    SERVICE_STATUS m_status = {};
    std::atomic<bool> m_running{false};
    static FaceService* s_pInstance;

    // Components
    std::unique_ptr<PipeServer> m_pipeServer;
    std::unique_ptr<WebcamCaptureDS> m_webcamDS; // standalone only
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

    bool m_isServiceMode = false;  // set by ServiceMain

    // Set when the system resumes from sleep/hibernate (PBT_APMRESUMESUSPEND).
    // Standalone capture can survive resume while its camera graph is stale;
    // force a fresh camera init on the next standalone auth. Service mode
    // always creates a new disposable child, so it has no retained camera.
    std::atomic<bool> m_resumedFlag{false};

    // Settings
    std::wstring m_dataDir;
    std::wstring m_modelsDir;
    float m_matchThreshold = 0.30f;
    int m_authTimeoutSeconds = 15;

    // --- Session-aware model residency ---
    std::shared_ptr<InferenceModels> m_models;
    // Service mode owns no ONNX or camera objects. It holds only the parent
    // control object for the short-lived worker; standalone retains the
    // in-process path for desktop development verification.
    std::shared_ptr<AuthWorkerClient> m_authWorker;
    ModelState m_modelState = ModelState::Unloaded;
    bool m_modelsWanted = false;
    bool m_modelLoadRequested = false;
    bool m_modelStopRequested = false;
    bool m_modelLowLightEnhance = false;
    std::wstring m_workerLoadError;
    // CONFIG_RELOAD may arrive while a child is preloading. A generation lets
    // the lifecycle thread discard that stale child and acknowledge only the
    // child that reflects the newly saved camera/model settings.
    uint64_t m_workerConfigGeneration = 0;
    uint64_t m_loadedWorkerConfigGeneration = 0;
    unsigned int m_activeModelUsers = 0;

    // Why the last model load failed, at category level: which model failed
    // an integrity (SHA-256) check vs a plain load error. Lets the lock
    // screen say "可能被篡改，重新安装" instead of a generic load error so a
    // tamper is visible, not just logged. Cleared when a complete bundle
    // loads successfully or the unlocked idle state intentionally drops the
    // models.
    std::atomic<ModelLoadFailure> m_modelLoadFailure{ModelLoadFailure::None};
    std::thread m_modelWorkerThread;
    std::mutex m_modelMutex;
    std::condition_variable m_modelCv;
};

} // namespace facelogin
