#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "liveness_types.h"
#include "onnx_models.h"
#include "auth_worker_client.h"
#include "webcam_capture.h"
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
    std::shared_ptr<InferenceModels> LoadInferenceModels(bool& padIntegrityFailed);
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
    std::unique_ptr<WebcamCapture> m_webcamMF; // standalone only
    std::unique_ptr<CredentialStore> m_store;

    // Configuration
    AppConfig m_config;
    LivenessMethod m_livenessMethod = LivenessMethod::AntiSpoof;
    float m_antiSpoofThreshold = 0.281f;

    bool m_isServiceMode = false;  // set by ServiceMain

    // Set when the system resumes from sleep/hibernate (PBT_APMRESUMESUSPEND).
    // Standalone MF can survive resume while its USB source reader is stale;
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

    // True only when the mandatory anti-spoof (PAD) model failed an integrity
    // (SHA-256) check — i.e. the file was tampered/corrupted, not merely a load
    // error. Lets ProcessAuthRequest tell the user "可能被篡改" instead of the
    // generic "模块不可用" so a tamper is visible on the lock screen, not just
    // in the log. Cleared when a complete bundle loads successfully or is
    // intentionally returned to the unlocked idle state.
    std::atomic<bool> m_padIntegrityFailed{false};
    std::thread m_modelWorkerThread;
    std::mutex m_modelMutex;
    std::condition_variable m_modelCv;
};

} // namespace facelogin
