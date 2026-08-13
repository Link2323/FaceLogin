#include "FaceService.h"
#include "auth_pipeline.h"
#include "performance_affinity.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/frame_image.h"
#include "../common/sha256_util.h"
#include "../common/model_hashes.h"
#include "../common/data_path.h"
#include "../common/secure_clear.h"
#include <shlobj.h>
#include <chrono>
#include <thread>
#include <vector>
#include <psapi.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace facelogin {

FaceService* FaceService::s_pInstance = nullptr;

static constexpr wchar_t SERVICE_NAME[] = L"FaceLoginService";

// UTF-8 → wide string, for passing config.camera_device to the camera backends.
static std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

struct ProcessResourceUsage {
    DWORD handles = 0;
    DWORD threads = 0;
    double privateMiB = 0.0;
    double workingSetMiB = 0.0;
};

static ProcessResourceUsage CurrentProcessResourceUsage() {
    ProcessResourceUsage usage;
    GetProcessHandleCount(GetCurrentProcess(), &usage.handles);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
                    ++usage.threads;
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        constexpr double kMiB = 1024.0 * 1024.0;
        usage.privateMiB = static_cast<double>(counters.PrivateUsage) / kMiB;
        usage.workingSetMiB = static_cast<double>(counters.WorkingSetSize) / kMiB;
    }
    return usage;
}

// Decide whether the service should preload models immediately. At the sign-in
// screen or an already-locked console we preserve cold-logon responsiveness;
// when the service starts while the desktop is already unlocked, it stays in
// the low-memory state until WTS_SESSION_LOCK (AUTH_REQUEST is the fallback).
static bool ShouldPreloadModelsAtStartup() {
    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) return true;

    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
                                    WTSSessionInfoEx, &buffer, &bytes)) {
        bool shouldPreload = true;
        const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
        if (bytes >= sizeof(WTSINFOEXW) && info->Level == 1) {
            const auto& level = info->Data.WTSInfoExLevel1;
            if (level.UserName[0] != L'\0') {
                if (level.SessionFlags == WTS_SESSIONSTATE_UNLOCK) {
                    shouldPreload = false;
                } else if (level.SessionFlags == WTS_SESSIONSTATE_LOCK) {
                    shouldPreload = true;
                } else {
                    shouldPreload = ReadRegDword(REGVAL_USER_LOGGED_IN, 0) == 0;
                }
            }
        }
        WTSFreeMemory(buffer);
        return shouldPreload;
    }

    // Fail toward availability when state cannot be queried. The registry is
    // only a hint and AUTH_REQUEST remains a second-chance load trigger.
    return ReadRegDword(REGVAL_USER_LOGGED_IN, 0) == 0;
}

static void SecureClearMatchPassword(std::optional<CredentialStore::MatchResult>& match) {
    if (match) {
        SecureClearWString(match->password);
    }
}

FaceService::FaceService() {
    s_pInstance = this;
}

FaceService::~FaceService() {
    s_pInstance = nullptr;
}

void WINAPI FaceService::ServiceMain(DWORD argc, LPWSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    FaceService service;
    service.m_isServiceMode = true;

    service.m_hStatus = RegisterServiceCtrlHandlerExW(
        SERVICE_NAME, HandlerEx, &service);

    if (!service.m_hStatus) {
        FACELOGIN_ERROR(L"RegisterServiceCtrlHandler failed: %lu", GetLastError());
        return;
    }

    service.m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service.m_status.dwCurrentState = SERVICE_START_PENDING;
    service.m_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        | SERVICE_ACCEPT_SESSIONCHANGE | SERVICE_ACCEPT_POWEREVENT;
    service.m_status.dwWin32ExitCode = NO_ERROR;
    service.m_status.dwServiceSpecificExitCode = 0;
    service.m_status.dwCheckPoint = 0;
    service.m_status.dwWaitHint = 10000;
    SetServiceStatus(service.m_hStatus, &service.m_status);

    if (!service.Initialize()) {
        service.m_status.dwCurrentState = SERVICE_STOPPED;
        service.m_status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        service.m_status.dwServiceSpecificExitCode = 1;
        SetServiceStatus(service.m_hStatus, &service.m_status);
        return;
    }

    service.m_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(service.m_hStatus, &service.m_status);

    FACELOGIN_INFO(L"FaceLoginService started");

    service.Run();
    // HandlerEx only requested the stop. Perform joins, job teardown and pipe
    // destruction on this service-main thread so SCM control delivery never
    // waits on a synchronous I/O operation.
    service.Stop();

    service.m_status.dwCurrentState = SERVICE_STOPPED;
    service.m_status.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(service.m_hStatus, &service.m_status);
    FACELOGIN_INFO(L"FaceLoginService stopped");
}

void FaceService::RunStandalone() {
    FaceService service;

    {
        // Resolve the log directory through the same secure resolver as
        // Initialize(). In standalone (development) mode this trusts the
        // EXE directory and %ProgramData%\FaceLogin, so dev workflows are
        // unaffected. A defensive fallback to the EXE directory keeps
        // logging alive even if the resolver unexpectedly rejects.
        std::wstring reason;
        std::wstring logDir = ResolveSecureDataDir(L"", &reason);
        if (logDir.empty()) {
            FACELOGIN_WARN(L"Standalone: secure data dir rejected (%s) — "
                           L"logging next to the EXE", reason.c_str());
            wchar_t exeBuf[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) != 0) {
                std::wstring exePath(exeBuf);
                size_t slash = exePath.find_last_of(L"\\/");
                logDir = (slash != std::wstring::npos) ? exePath.substr(0, slash) : L".";
            } else {
                logDir = L".";
            }
        }
        CreateDirectoryW(logDir.c_str(), nullptr);
        std::wstring logPath = logDir + L"\\log\\service.log";
        Logger::Instance().SetLogFile(logPath);
    }
    Logger::Instance().SetMinLevel(LogLevel::Debug);
    Logger::Instance().SetEnableDebugOutput(true);
    FACELOGIN_INFO(L"=== FaceLoginService standalone mode ===");

    if (!service.Initialize()) {
        FACELOGIN_ERROR(L"Initialization failed");
        return;
    }

    service.Run();
}

DWORD WINAPI FaceService::HandlerEx(DWORD control, DWORD eventType,
                                     LPVOID eventData, LPVOID context) {

    auto* pService = static_cast<FaceService*>(context);

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        pService->m_status.dwCurrentState = SERVICE_STOP_PENDING;
        pService->m_status.dwControlsAccepted = 0;
        pService->m_status.dwWaitHint = 20000;
        pService->m_status.dwCheckPoint = 1;
        SetServiceStatus(pService->m_hStatus, &pService->m_status);
        pService->RequestStop();
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT: {
        // PBT_APMRESUMESUSPEND = resumed from sleep/hibernation. The USB camera
        // may still be in low-power recovery, so force a fresh camera init on
        // the next auth instead of reusing a stale SourceReader.
        //
        // Service mode has no parent-owned camera and starts a fresh child on
        // every authentication, so this flag is consumed only by standalone.
        if (eventType == PBT_APMRESUMESUSPEND) {
            FACELOGIN_INFO(L"Power resume event — forcing camera re-init on next auth");
            pService->m_resumedFlag.store(true);
        }
        return NO_ERROR;
    }
    case SERVICE_CONTROL_SESSIONCHANGE: {
        // Session notifications drive model residency. HandlerEx only queues a
        // lifecycle request; the model worker performs all heavy work.
        auto* evt = reinterpret_cast<WTSSESSION_NOTIFICATION*>(eventData);
        if (evt && evt->cbSize == sizeof(WTSSESSION_NOTIFICATION)
            && evt->dwSessionId == WTSGetActiveConsoleSessionId()) {
            if (eventType == WTS_SESSION_LOGON) {
                FACELOGIN_INFO(L"Session LOGON: session=%lu → UserLoggedIn=1",
                              evt->dwSessionId);
                WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
                pService->RequestModelUnload(L"session logon");
            } else if (eventType == WTS_SESSION_LOGOFF) {
                FACELOGIN_INFO(L"Session LOGOFF: session=%lu → UserLoggedIn=0",
                              evt->dwSessionId);
                // UserLoggedIn=0 drives the service's own model-preload
                // decision on the next logon (ShouldPreloadModels) — the
                // credential provider no longer reads it (cold-boot auto-
                // trigger removed 2026-08; recognition waits for input).
                WriteRegDword(REGVAL_USER_LOGGED_IN, 0);
                pService->RequestModelLoad(L"session logoff");
            } else if (eventType == WTS_SESSION_LOCK) {
                FACELOGIN_INFO(L"Session LOCK: session=%lu → preloading models",
                              evt->dwSessionId);
                pService->RequestModelLoad(L"session lock");
            } else if (eventType == WTS_SESSION_UNLOCK) {
                FACELOGIN_INFO(L"Session UNLOCK: session=%lu → releasing models",
                              evt->dwSessionId);
                pService->RequestModelUnload(L"session unlock");
            } else {
                // Connect/disconnect and remote-session events are not part of
                // the active-console face-auth lifecycle.
                FACELOGIN_DEBUG(L"Session change ignored: eventType=%lu", eventType);
            }
        }
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(pService->m_hStatus, &pService->m_status);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

bool FaceService::Initialize() {
    // Resolve the data directory through the security whitelist check
    // (security #3 — DataPath registry redirection defense). The registry
    // DataPath value is trusted only if it points at a allow-listed location
    // (EXE dir in production; EXE dir / ProgramData / hint in development).
    // A rejected path is fail-closed: the service refuses to start rather
    // than load models / users.dat from an attacker-controlled directory.
    {
        std::wstring reason;
        m_dataDir = ResolveSecureDataDir(L"", &reason);
        if (m_dataDir.empty()) {
            FACELOGIN_ERROR(L"Refusing to start — data directory rejected: %s",
                            reason.c_str());
            return false;
        }
    }
    CreateDirectoryW(m_dataDir.c_str(), nullptr);
    m_modelsDir = m_dataDir + L"\\models";

    // Load configuration from config.json (falls back to registry). Must happen
    // before camera init — the configured camera_device is used below.
    m_config = LoadConfig(m_dataDir);
    m_matchThreshold = m_config.match_threshold;
    m_livenessMethod = m_config.liveness_method;
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;

    // Blink liveness was removed with the dlib 68-point model (v1.5). Configs
    // that still say "blink" are mapped to the silent anti-spoof path.
    if (m_livenessMethod == LivenessMethod::Blink) {
        FACELOGIN_WARN(L"liveness_method=blink no longer supported (dlib 68-point removed) — using anti-spoof");
        m_livenessMethod = LivenessMethod::AntiSpoof;
    }

    std::wstring logPath = m_dataDir + L"\\log\\service.log";
    Logger::Instance().SetLogFile(logPath);
    Logger::Instance().SetMinLevel(LogLevel::Info);
    FACELOGIN_INFO(L"=== FaceLoginService initializing ===");
    FACELOGIN_INFO(L"Data dir: %s", m_dataDir.c_str());
    FACELOGIN_INFO(L"Models dir: %s", m_modelsDir.c_str());

    m_store = std::make_unique<CredentialStore>();
    m_store->SetDataDir(m_dataDir);
    if (!m_store->LoadDatabase()) {
        FACELOGIN_ERROR(L"Failed to load credential database");
        return false;
    }
    FACELOGIN_INFO(L"Loaded %zu registered user(s)", m_store->GetUserCount());

    m_pipeServer = std::make_unique<PipeServer>();

    if (m_isServiceMode) {
        // The parent service never opens a camera or an ONNX session. The
        // preloaded child does so for one request, then exits to reclaim driver
        // ETW registrations and ORT/Windows heap high-water deterministically.
        FACELOGIN_INFO(L"Authentication worker will initialize %s on demand%s",
                       m_config.camera_backend == "mf" ? L"Media Foundation" : L"DirectShow",
                       m_config.camera_device.empty() ? L"" : L" (configured device)");
    } else {
        // Media Foundation camera is also initialized on demand — keeping
        // it open across auth sessions causes the source reader to stall
        // (especially when FaceLoginConsole is running concurrently).
        FACELOGIN_INFO(L"MF webcam will be initialized on demand%s",
                       m_config.camera_device.empty() ? L"" : L" (configured device)");
    }

    // dlib recognizer/detector were removed — the system is now pure ONNX.
    // recognition_model/detector config values are ignored (only onnx/scrfd
    // are supported; anything else logs a warning for backwards compat).

    // Defensive invariant: config parsing maps legacy "none" to anti-spoof,
    // and legacy blink was normalized above.  Do not permit any future config
    // path to reintroduce identity-only authentication.
    if (m_livenessMethod != LivenessMethod::AntiSpoof) {
        FACELOGIN_ERROR(L"No supported liveness method configured — refusing to start");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_modelMutex);
        m_modelLowLightEnhance = m_config.low_light_enhance;
    }
    if (!StartModelWorker()) {
        FACELOGIN_ERROR(L"Failed to start model lifecycle worker");
        return false;
    }

    const bool preload = !m_isServiceMode || ShouldPreloadModelsAtStartup();
    if (preload) {
        RequestModelLoad(L"service startup / sign-in screen");
    } else {
        FACELOGIN_INFO(L"Unlocked desktop detected at startup — models remain unloaded until lock");
    }

    FACELOGIN_INFO(L"Liveness method: antispoof (mandatory, fail-closed; session-aware model residency)");
    FACELOGIN_INFO(L"Match threshold: %.3f", m_matchThreshold);
    FACELOGIN_INFO(L"Initialization complete (pipe listening; model worker ready)");

    return true;
}

// ============================================================================
// Session-aware model residency
// ============================================================================

std::shared_ptr<FaceService::InferenceModels>
FaceService::LoadInferenceModels(bool& padIntegrityFailed) {
    padIntegrityFailed = false;
    FACELOGIN_INFO(L"Loading inference models in background...");

    auto models = std::make_shared<InferenceModels>();

    // 1. SCRFD detector (gnkps variant with five alignment keypoints).
    models->detector = std::make_unique<OnnxDetector>();
    const std::wstring detectorPath = m_modelsDir + L"\\det_10g_gnkps.onnx";
    if (!VerifyModelIntegrity(detectorPath, model_hashes::kDetector, L"SCRFD detector")) {
        FACELOGIN_ERROR(L"SCRFD detector integrity check failed — face detection unavailable");
        return {};
    }
    if (!models->detector->Initialize(detectorPath)) {
        FACELOGIN_ERROR(L"SCRFD detector failed to load — face detection unavailable");
        return {};
    }
    FACELOGIN_INFO(L"SCRFD detector loaded");

    // 2. InsightFace recognizer (the largest resident model).
    models->recognizer = std::make_unique<OnnxRecognizer>();
    const std::wstring recognizerPath = m_modelsDir + L"\\w600k_r50.onnx";
    if (!VerifyModelIntegrity(recognizerPath, model_hashes::kRecognizer,
                              L"InsightFace w600k_r50 recognizer")) {
        FACELOGIN_ERROR(L"ONNX recognizer integrity check failed — recognition unavailable");
        return {};
    }
    if (!models->recognizer->Initialize(recognizerPath)) {
        FACELOGIN_ERROR(L"ONNX recognizer failed to load — recognition unavailable");
        return {};
    }
    FACELOGIN_INFO(L"ONNX recognizer loaded — using InsightFace w600k_r50");

    // 3. Dual MiniFAS PAD. Never publish a partial bundle: authentication is
    // fail-closed unless detector, recognizer and both PAD sessions are ready.
    models->antiSpoof = std::make_unique<OnnxAntiSpoof>();
    const std::wstring miniFasV2Path = m_modelsDir + L"\\MiniFASNetV2.onnx";
    const std::wstring miniFasV1SePath = m_modelsDir + L"\\MiniFASNetV1SE.onnx";
    if (!VerifyModelIntegrity(miniFasV2Path, model_hashes::kMiniFasV2, L"MiniFASNetV2 (PAD)") ||
        !VerifyModelIntegrity(miniFasV1SePath, model_hashes::kMiniFasV1Se, L"MiniFASNetV1SE (PAD)")) {
        padIntegrityFailed = true;
        FACELOGIN_ERROR(L"Anti-spoof model integrity check failed — authentication "
                        L"will remain disabled (fail-closed)");
        return {};
    }
    if (!models->antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath)) {
        FACELOGIN_ERROR(L"Anti-spoof model unavailable — authentication will remain disabled");
        return {};
    }

    FACELOGIN_INFO(L"Anti-spoof models loaded (MiniFASNetV2 + MiniFASNetV1SE)");
    FACELOGIN_INFO(L"All inference models loaded");
    return models;
}

bool FaceService::StartModelWorker() {
    try {
        m_modelWorkerThread = std::thread(&FaceService::ModelWorkerLoop, this);
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"Model worker start failed: %hs", e.what());
        return false;
    }
}

void FaceService::RequestModelLoad(const wchar_t* reason) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_modelMutex);
        if (m_modelStopRequested) return;
        m_modelsWanted = true;
        if (m_modelState == ModelState::Unloaded ||
            m_modelState == ModelState::Failed) {
            if (!m_modelLoadRequested) {
                m_modelLoadRequested = true;
                queued = true;
            }
        }
    }
    m_modelCv.notify_all();
    if (queued) FACELOGIN_INFO(L"Model load requested: %s", reason);
}

void FaceService::RequestModelUnload(const wchar_t* reason) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_modelMutex);
        if (m_modelStopRequested) return;
        queued = m_modelsWanted || m_modelState != ModelState::Unloaded;
        m_modelsWanted = false;
        m_modelLoadRequested = false;
    }
    m_modelCv.notify_all();
    if (queued) FACELOGIN_INFO(L"Model unload requested: %s", reason);
}

void FaceService::BeginModelUse() {
    std::lock_guard<std::mutex> lock(m_modelMutex);
    ++m_activeModelUsers;
}

void FaceService::EndModelUse() {
    {
        std::lock_guard<std::mutex> lock(m_modelMutex);
        if (m_activeModelUsers > 0) --m_activeModelUsers;
    }
    m_modelCv.notify_all();
}

std::shared_ptr<FaceService::InferenceModels> FaceService::AcquireModelsForAuth() {
    // AUTH_REQUEST is the fail-safe if WTS_SESSION_LOCK was delayed or missed.
    RequestModelLoad(L"AUTH_REQUEST fallback");

    std::unique_lock<std::mutex> lock(m_modelMutex);
    m_modelCv.wait(lock, [this]() {
        return m_modelState == ModelState::Ready ||
               m_modelState == ModelState::Failed ||
               m_modelState == ModelState::Stopping ||
               !m_modelsWanted;
    });
    return m_modelState == ModelState::Ready ? m_models : nullptr;
}

std::shared_ptr<AuthWorkerClient> FaceService::LoadAuthenticationWorker(
    const AppConfig& appConfig, std::wstring& errorMessage) {
    auth_worker::WorkerConfig workerConfig;
    // The production service remains in Session 0 and starts with the proven
    // DirectShow backend. Media Foundation is selected only by the later A/B
    // configuration gate; moving into a child process does not itself change
    // the Windows session or camera-driver compatibility.
    workerConfig.cameraBackend = appConfig.camera_backend == "mf"
        ? auth_worker::CameraBackend::MediaFoundation
        : auth_worker::CameraBackend::DirectShow;
    workerConfig.cameraRotation = appConfig.camera_rotation;
    workerConfig.antiSpoofThreshold = appConfig.anti_spoof_threshold;
    workerConfig.authTimeoutSeconds = m_authTimeoutSeconds;
    workerConfig.lowLightEnhance = appConfig.low_light_enhance;
    workerConfig.cameraDevice = Utf8ToWstr(appConfig.camera_device);

    auto worker = std::make_shared<AuthWorkerClient>(std::move(workerConfig));
    if (!worker->Start(errorMessage)) return {};
    return worker;
}

std::shared_ptr<AuthWorkerClient> FaceService::AcquireAuthWorkerForAuth() {
    // AUTH_REQUEST is the fail-safe if WTS_SESSION_LOCK was delayed or missed.
    RequestModelLoad(L"AUTH_REQUEST worker fallback");

    std::unique_lock<std::mutex> lock(m_modelMutex);
    m_modelCv.wait(lock, [this]() {
        return m_modelState == ModelState::Ready ||
               m_modelState == ModelState::Failed ||
               m_modelState == ModelState::Stopping ||
               !m_modelsWanted;
    });
    return m_modelState == ModelState::Ready ? m_authWorker : nullptr;
}

void FaceService::MarkAuthWorkerConsumed(bool preloadReplacement) {
    std::shared_ptr<AuthWorkerClient> consumed;
    {
        std::lock_guard<std::mutex> lock(m_modelMutex);
        consumed = std::move(m_authWorker);
        if (m_modelState != ModelState::Stopping) {
            m_modelState = ModelState::Unloaded;
            if (preloadReplacement && m_modelsWanted && !m_modelStopRequested) {
                m_modelLoadRequested = true;
            }
        }
    }
    // Authenticate() has already terminated the child after its terminal
    // message. Reset outside the lifecycle lock so the Job close cannot block
    // a WTS handler or model state transition.
    consumed.reset();
    m_modelCv.notify_all();
}

void FaceService::ModelWorkerLoop() {
    for (;;) {
        std::shared_ptr<InferenceModels> modelsToRelease;
        std::shared_ptr<AuthWorkerClient> workerToRelease;

        std::unique_lock<std::mutex> lock(m_modelMutex);
        m_modelCv.wait(lock, [this]() {
            const bool canUnload = !m_modelsWanted &&
                m_modelState != ModelState::Unloaded &&
                m_modelState != ModelState::Loading &&
                m_activeModelUsers == 0;
            return m_modelStopRequested || m_modelLoadRequested || canUnload;
        });

        if (m_modelStopRequested) {
            if (m_isServiceMode) workerToRelease = std::move(m_authWorker);
            else modelsToRelease = std::move(m_models);
            m_modelState = ModelState::Stopping;
            m_modelCv.notify_all();
            lock.unlock();
            modelsToRelease.reset();
            workerToRelease.reset();
            return;
        }

        if (!m_modelsWanted && m_modelState != ModelState::Loading &&
            m_activeModelUsers == 0) {
            if (m_isServiceMode) workerToRelease = std::move(m_authWorker);
            else modelsToRelease = std::move(m_models);
            m_modelState = ModelState::Unloaded;
            m_padIntegrityFailed.store(false);
            m_modelCv.notify_all();
            lock.unlock();
            modelsToRelease.reset();
            workerToRelease.reset();
            const auto usage = CurrentProcessResourceUsage();
            if (m_isServiceMode) {
                FACELOGIN_INFO(L"Authentication worker released (unlocked idle state; parent handles=%lu, private=%.1f MiB, working_set=%.1f MiB, threads=%lu)",
                               usage.handles, usage.privateMiB, usage.workingSetMiB,
                               usage.threads);
            } else {
                FACELOGIN_INFO(L"Inference models released (unlocked idle state; handles=%lu, private=%.1f MiB, working_set=%.1f MiB)",
                               usage.handles, usage.privateMiB, usage.workingSetMiB);
            }
            continue;
        }

        if (!m_modelLoadRequested || !m_modelsWanted) continue;

        m_modelLoadRequested = false;
        m_modelState = ModelState::Loading;
        m_modelCv.notify_all();
        lock.unlock();

        const auto loadStart = std::chrono::steady_clock::now();
        bool padIntegrityFailed = false;
        std::shared_ptr<InferenceModels> loaded;
        std::shared_ptr<AuthWorkerClient> loadedWorker;
        std::wstring workerError;
        AppConfig workerConfigSnapshot;
        uint64_t workerConfigGeneration = 0;

        // A pending replacement is always one-shot in service mode. Stop the
        // previous child before opening a new one so it cannot retain a camera
        // graph or model heap beside the replacement.
        if (m_isServiceMode) {
            lock.lock();
            workerToRelease = std::move(m_authWorker);
            workerConfigSnapshot = m_config;
            workerConfigGeneration = m_workerConfigGeneration;
            lock.unlock();
            workerToRelease.reset();
        }
        try {
            if (m_isServiceMode) {
                loadedWorker = LoadAuthenticationWorker(workerConfigSnapshot, workerError);
            }
            else loaded = LoadInferenceModels(padIntegrityFailed);
        } catch (const std::exception& e) {
            if (m_isServiceMode) {
                FACELOGIN_ERROR(L"Authentication worker launcher threw: %hs", e.what());
            } else {
                FACELOGIN_ERROR(L"Model loader threw: %hs", e.what());
            }
            workerError = L"认证工作进程启动异常";
        }

        bool published = false;
        bool wantedAfterLoad = false;
        lock.lock();
        if (m_modelStopRequested) {
            m_modelState = ModelState::Stopping;
        } else if (m_isServiceMode && loadedWorker && m_modelsWanted) {
            m_authWorker = loadedWorker;
            m_modelState = ModelState::Ready;
            m_padIntegrityFailed.store(false);
            m_workerLoadError.clear();
            m_loadedWorkerConfigGeneration = workerConfigGeneration;
            published = true;
        } else if (!m_isServiceMode && loaded && m_modelsWanted) {
            loaded->recognizer->SetLowLightEnhance(m_modelLowLightEnhance);
            m_models = loaded;
            m_modelState = ModelState::Ready;
            m_padIntegrityFailed.store(false);
            published = true;
        } else if (!m_modelsWanted) {
            m_modelState = ModelState::Unloaded;
            m_padIntegrityFailed.store(false);
        } else {
            m_modelState = ModelState::Failed;
            m_padIntegrityFailed.store(padIntegrityFailed);
            if (m_isServiceMode) {
                m_workerLoadError = workerError.empty()
                    ? L"认证工作进程启动失败，请使用密码登录"
                    : workerError;
            }
        }
        wantedAfterLoad = m_modelsWanted;
        m_modelCv.notify_all();
        lock.unlock();

        const double loadMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - loadStart).count();
        if (published) {
            const auto usage = CurrentProcessResourceUsage();
            if (m_isServiceMode) {
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
            if (m_isServiceMode) {
                FACELOGIN_INFO(L"Authentication worker load not published (wanted=%d, %.0f ms)",
                               wantedAfterLoad ? 1 : 0, loadMs);
            } else {
                FACELOGIN_INFO(L"Inference model load not published (wanted=%d, %.0f ms)",
                               wantedAfterLoad ? 1 : 0, loadMs);
            }
        }
    }
}

void FaceService::StopModelWorker() {
    {
        std::lock_guard<std::mutex> lock(m_modelMutex);
        m_modelStopRequested = true;
        m_modelsWanted = false;
        m_modelLoadRequested = false;
    }
    m_modelCv.notify_all();
    if (m_modelWorkerThread.joinable()) m_modelWorkerThread.join();
}

void FaceService::Run() {
    m_running = true;

    while (m_running) {
        if (!m_pipeServer->WaitForClient(60000)) {
            if (!m_running) break;
            continue;
        }

        std::wstring request;
        if (!m_pipeServer->ReadMessage(request, 30000)) {
            m_pipeServer->Disconnect();
            continue;
        }

        FACELOGIN_INFO(L"Received request: %s", request.c_str());

        if (request == ipc::MSG_RELOAD_DB) {
            // Honor LoadDatabase()'s return value: a corrupt users.dat (bad
            // magic, length fields out of range, truncated read) returns false
            // and clears the in-memory user list. Reporting success here would
            // leave the service with an empty database while logging "reloaded",
            // causing every subsequent auth to fail as "no registered users"
            // until the service is restarted. Surface the failure instead.
            if (m_store->LoadDatabase()) {
                m_pipeServer->WriteMessage(ipc::MSG_RELOAD_OK);
                m_pipeServer->Disconnect();
                FACELOGIN_INFO(L"Database reloaded");
            } else {
                m_pipeServer->WriteMessage(ipc::MSG_CONFIG_RELOAD_ERROR);
                m_pipeServer->Disconnect();
                FACELOGIN_ERROR(L"Database reload failed — users.dat parse error; "
                                L"previous in-memory database retained");
            }
        }
        else if (request == ipc::MSG_CONFIG_RELOAD) {
            AppConfig reloadedConfig = LoadConfig(m_dataDir);
            LivenessMethod reloadedLiveness = reloadedConfig.liveness_method;

            // dlib recognizer/detector were removed — recognition_model and
            // detector config values are ignored (pure ONNX now).
            if (reloadedLiveness == LivenessMethod::Blink) {
                FACELOGIN_WARN(L"liveness_method=blink no longer supported — using anti-spoof");
                reloadedLiveness = LivenessMethod::AntiSpoof;
            }
            if (reloadedLiveness != LivenessMethod::AntiSpoof) {
                FACELOGIN_WARN(L"CONFIG_RELOAD: unsupported liveness method rejected — enforcing anti-spoof");
                reloadedLiveness = LivenessMethod::AntiSpoof;
            }

            // CONFIG_RELOAD must not wake intentionally-unloaded models on an
            // unlocked desktop. In service mode every camera/model setting is
            // part of the child construction snapshot, so request a fresh
            // worker when the lock screen is active. A generation prevents a
            // child that was already loading from being acknowledged as the
            // newly configured backend.
            std::shared_ptr<InferenceModels> loadedModels;
            bool waitForConfiguredWorker = false;
            uint64_t requiredWorkerGeneration = 0;
            {
                std::lock_guard<std::mutex> lock(m_modelMutex);
                m_config = std::move(reloadedConfig);
                m_matchThreshold = m_config.match_threshold;
                m_livenessMethod = reloadedLiveness;
                m_antiSpoofThreshold = m_config.anti_spoof_threshold;
                m_modelLowLightEnhance = m_config.low_light_enhance;
                if (m_isServiceMode) {
                    ++m_workerConfigGeneration;
                    requiredWorkerGeneration = m_workerConfigGeneration;
                    if (m_modelsWanted && m_modelState != ModelState::Stopping) {
                        m_modelLoadRequested = true;
                        waitForConfiguredWorker = true;
                    }
                } else {
                    if (m_modelState == ModelState::Ready) {
                        loadedModels = m_models;
                    } else if (m_modelsWanted && m_modelState == ModelState::Failed) {
                        m_modelLoadRequested = true;
                        waitForConfiguredWorker = true;
                    }
                }
            }
            m_modelCv.notify_all();
            if (loadedModels) {
                // PAD remains on its calibrated raw-camera preprocessing path.
                loadedModels->recognizer->SetLowLightEnhance(m_config.low_light_enhance);
            }

            bool modelStateOk = true;
            if (waitForConfiguredWorker) {
                std::unique_lock<std::mutex> lock(m_modelMutex);
                m_modelCv.wait(lock, [this, requiredWorkerGeneration]() {
                    return m_modelState == ModelState::Ready ||
                           m_modelState == ModelState::Failed ||
                           m_modelState == ModelState::Stopping ||
                           !m_modelsWanted;
                });
                if (m_isServiceMode && m_modelState == ModelState::Ready &&
                    m_loadedWorkerConfigGeneration < requiredWorkerGeneration) {
                    // The first child was started before CONFIG_RELOAD. Wait
                    // for the queued replacement, not merely any Ready state.
                    m_modelCv.wait(lock, [this, requiredWorkerGeneration]() {
                        return (m_modelState == ModelState::Ready &&
                                m_loadedWorkerConfigGeneration >= requiredWorkerGeneration) ||
                               m_modelState == ModelState::Failed ||
                               m_modelState == ModelState::Stopping ||
                               !m_modelsWanted;
                    });
                }
                modelStateOk = !m_modelsWanted ||
                    (m_modelState == ModelState::Ready &&
                     (!m_isServiceMode ||
                      m_loadedWorkerConfigGeneration >= requiredWorkerGeneration));
            }

            m_pipeServer->WriteMessage(modelStateOk
                ? ipc::MSG_CONFIG_RELOAD_OK
                : ipc::MSG_CONFIG_RELOAD_ERROR);
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Configuration reloaded: rec=%hs det=%hs live=%hs thr=%.2f backend=%hs rotation=%d",
                          m_config.recognition_model.c_str(), m_config.detector.c_str(),
                          "antispoof",
                           m_matchThreshold, m_config.camera_backend.c_str(),
                           m_config.camera_rotation);
        }
        else if (request == ipc::MSG_GET_LOGS) {
            auto lines = Logger::Instance().GetRecentLogs(500);
            std::wstring resp(ipc::MSG_GET_LOGS_OK_PREFIX);
            for (size_t i = 0; i < lines.size(); i++) {
                if (i > 0) resp += L"\x1E"; // ASCII record separator
                // Escape backslashes and the separator char itself
                std::wstring safe = lines[i];
                // Remove trailing \r\n from each line
                while (!safe.empty() && (safe.back() == L'\r' || safe.back() == L'\n'))
                    safe.pop_back();
                resp += safe;
            }
            m_pipeServer->WriteMessage(resp);
            m_pipeServer->Disconnect();
            FACELOGIN_DEBUG(L"Sent %zu log lines to client", lines.size());
        }
        else if (request == ipc::MSG_AUTH_REQUEST) {
            if (m_isServiceMode) {
                // Service mode owns no camera/ONNX objects. The preloaded
                // child creates its graph only for this request and exits after
                // the terminal result, which releases driver ETW registrations
                // and ORT/Windows heap high-water with the process.
                ProcessAuthRequest();
                m_pipeServer->Disconnect();
                continue;
            }

            // Standalone keeps the in-process MF path for desktop development
            // and enrollment-adjacent diagnostics. Production always uses the
            // worker branch above.
            {
                // After a system resume the camera may still be in low-power
                // recovery. Drop the stale instance so Initialize() rebuilds a
                // fresh SourceReader instead of reusing the one that stalled.
                if (m_resumedFlag.exchange(false) && m_webcamMF) {
                    FACELOGIN_INFO(L"Resume detected — rebuilding MF camera");
                    m_webcamMF->Shutdown();
                    m_webcamMF.reset();
                }
                if (!m_webcamMF) {
                    m_webcamMF = std::make_unique<WebcamCapture>();
                    if (!m_webcamMF->Initialize(640, 480, Utf8ToWstr(m_config.camera_device))) {
                        FACELOGIN_ERROR(L"MF camera init failed on demand");
                        m_webcamMF.reset();
                        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"摄像头不可用"));
                        FlushFileBuffers(m_pipeServer->GetHandle());
                        m_pipeServer->DrainOutput(5000);
                        m_pipeServer->Disconnect();
                        continue;
                    }
                    FACELOGIN_INFO(L"MF camera initialized on demand for auth");
                }
            }
            ProcessAuthRequest();
            if (m_webcamMF) {
                m_webcamMF->Shutdown();
                m_webcamMF.reset();
                const auto usage = CurrentProcessResourceUsage();
                FACELOGIN_INFO(L"MF camera released after auth (handles=%lu)",
                               usage.handles);
            }
            m_pipeServer->Disconnect();
        }
        else if (request == ipc::MSG_PING) {
            m_pipeServer->WriteMessage(ipc::MSG_PONG);
            m_pipeServer->Disconnect();
        }
        else {
            FACELOGIN_WARN(L"Unknown request: %s", request.c_str());
            m_pipeServer->Disconnect();
        }
    }
}

// Session events arrive via HandlerEx → SERVICE_CONTROL_SESSIONCHANGE.
// eventType carries WTS_SESSION_LOGON / WTS_SESSION_LOGOFF.
// eventData is a WTSSESSION_NOTIFICATION with the session ID.
// We only care about the console session (session 1).

void FaceService::Stop() {
    m_running = false;
    if (m_pipeServer) {
        m_pipeServer->RequestShutdown();
    }
    StopModelWorker();
    if (!m_isServiceMode && m_webcamMF) {
        m_webcamMF->Shutdown();
    }
    if (m_pipeServer) {
        m_pipeServer->Close();
    }
}

void FaceService::RequestStop() {
    m_running = false;
    if (m_pipeServer) {
        m_pipeServer->RequestShutdown();
    }
}

bool FaceService::ProcessWorkerAuthRequest() {
    FACELOGIN_INFO(L"Starting face authentication through worker...");

    if (m_store->GetUserCount() == 0) {
        FACELOGIN_WARN(L"No registered users");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"没有注册用户"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    std::optional<CredentialStore::IdentityMatch> lockedIdentity;
    std::wstring initialSid;
    AuthWorkerResult workerResult;

    {
        // Prevent SESSION_UNLOCK from killing the worker between the public
        // request and its terminal response. The worker itself owns all model
        // and camera objects; this guard protects only the parent control
        // channel and lifecycle state.
        const ModelUseGuard workerUse(*this);

        bool workerNeedsWait = false;
        {
            std::lock_guard<std::mutex> lock(m_modelMutex);
            workerNeedsWait = m_modelState != ModelState::Ready ||
                !m_authWorker || !m_authWorker->IsReady();
            if (m_modelState == ModelState::Ready &&
                (!m_authWorker || !m_authWorker->IsReady())) {
                // A one-shot worker may have exited after a previous result
                // before Windows emitted SESSION_UNLOCK. Treat it as unloaded
                // and let the ordinary lifecycle thread make a fresh one.
                m_modelState = ModelState::Unloaded;
                m_modelLoadRequested = true;
            }
        }
        if (workerNeedsWait) {
            m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) +
                                       L"正在加载模型...");
            FlushFileBuffers(m_pipeServer->GetHandle());
        }
        m_modelCv.notify_all();

        auto worker = AcquireAuthWorkerForAuth();
        if (!worker || !worker->IsReady()) {
            std::wstring loadError;
            {
                std::lock_guard<std::mutex> lock(m_modelMutex);
                loadError = m_workerLoadError;
            }
            FACELOGIN_ERROR(L"Authentication worker is not ready");
            workerResult.errorMessage = loadError.empty()
                ? L"认证工作进程加载失败，请使用密码登录"
                : loadError;
        } else {
            AuthWorkerCallbacks callbacks;
            callbacks.reportStatus = [this](const std::wstring& text) {
                m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + text);
                FlushFileBuffers(m_pipeServer->GetHandle());
            };
            callbacks.isCancelled = [this]() {
                return !m_running || m_pipeServer->IsClientDisconnected();
            };
            callbacks.verifyBinding = [this, &lockedIdentity, &initialSid](
                const std::vector<float>& embedding, unsigned int bindingIndex) {
                auto identity = m_store->FindBestIdentity(embedding.data(), embedding.size(),
                                                          m_matchThreshold);
                if (!identity) return BindingDecision{BindingDecisionKind::Retry, {}};

                if (bindingIndex == 0) {
                    if (lockedIdentity) {
                        return BindingDecision{BindingDecisionKind::Reject,
                                               L"身份验证状态异常，请使用密码登录"};
                    }
                    if (identity->sid.empty()) {
                        FACELOGIN_ERROR(L"Matched credential has an empty SID — enrollment data is invalid");
                        return BindingDecision{BindingDecisionKind::Reject,
                                               L"身份数据无效，请使用密码登录并重新录入人脸"};
                    }
                    if (identity->passwordless) {
                        FACELOGIN_WARN(L"Matched passwordless account '%s' — face login cannot unlock",
                                       identity->username.c_str());
                        return BindingDecision{BindingDecisionKind::Reject,
                                               ipc::MSG_PASSWORDLESS_NOTICE};
                    }
                    initialSid = identity->sid;
                    lockedIdentity = std::move(identity);
                    return BindingDecision{BindingDecisionKind::Accept, {}};
                }

                if (!lockedIdentity || bindingIndex > 2 || identity->sid.empty() ||
                    identity->sid != initialSid) {
                    FACELOGIN_WARN(L"Identity changed during anti-spoof — rejecting face swap");
                    return BindingDecision{BindingDecisionKind::Reject,
                                           L"活体验证期间人脸不匹配，请重试"};
                }
                return BindingDecision{BindingDecisionKind::Accept, {}};
            };
            workerResult = worker->Authenticate(std::move(callbacks));
        }
    }

    // Every worker has handled at most one request. The replacement decision
    // follows the complete parent-side result: a valid worker success can
    // still fail closed if the SID/credential disappeared before the single
    // final decrypt or if the public pipe breaks. In those cases preload a
    // fresh worker only when the lifecycle still says the desktop is locked.
    if (workerResult.cancelled) {
        MarkAuthWorkerConsumed(true);
        return false;
    }
    if (workerResult.timedOut) {
        MarkAuthWorkerConsumed(true);
        FACELOGIN_INFO(L"Authentication timed out in worker");
        m_pipeServer->WriteMessage(ipc::MSG_AUTH_TIMEOUT);
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }
    if (!workerResult.succeeded) {
        MarkAuthWorkerConsumed(true);
        const std::wstring message = workerResult.errorMessage.empty()
            ? L"认证工作进程执行失败，请使用密码登录"
            : workerResult.errorMessage;
        FACELOGIN_WARN(L"Authentication worker failed: %s", message.c_str());
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(message));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }
    if (!lockedIdentity) {
        MarkAuthWorkerConsumed(true);
        FACELOGIN_ERROR(L"Worker reported success without a parent-bound identity");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
            L"身份验证状态异常，请使用密码登录"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    auto credential = m_store->LoadCredentialForSid(lockedIdentity->sid,
                                                     lockedIdentity->distance);
    if (!credential || credential->passwordless) {
        MarkAuthWorkerConsumed(true);
        FACELOGIN_ERROR(L"Authorized identity could not provide a password credential");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
            credential && credential->passwordless
                ? ipc::MSG_PASSWORDLESS_NOTICE
                : L"账户凭据不可用，请使用密码登录"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        SecureClearMatchPassword(credential);
        return false;
    }

    std::wstring domain = L".";
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = ARRAYSIZE(computerName);
    if (GetComputerNameW(computerName, &size)) domain = computerName;

    std::wstring message = ipc::BuildAuthSuccessMessage(
        credential->sid, credential->upn, domain, credential->username,
        credential->password);
    const bool writeOk = m_pipeServer->WriteMessage(message);
    FlushFileBuffers(m_pipeServer->GetHandle());
    SecureClearWString(message);
    if (!writeOk) {
        MarkAuthWorkerConsumed(true);
        FACELOGIN_WARN(L"Failed to send authentication credentials");
        SecureClearMatchPassword(credential);
        return false;
    }

    FACELOGIN_INFO(L"Credentials sent for %s\\%s", domain.c_str(),
                   credential->username.c_str());
    FACELOGIN_INFO(L"Identity binding complete: user=%s, distance=%.4f, same_sid=3/3",
                   credential->username.c_str(), lockedIdentity->distance);
    if (workerResult.hasTiming) {
        FACELOGIN_INFO(L"Authentication worker timing: camera_init=%.1f ms, pipeline=%.1f ms, total=%.1f ms; supervisor=%.1f ms, cleanup=%.1f ms",
                       workerResult.timing.cameraInitMs,
                       workerResult.timing.pipelineMs,
                       workerResult.timing.totalMs,
                       workerResult.supervisorElapsedMs,
                       workerResult.cleanupMs);
    }
    SecureClearMatchPassword(credential);
    // A successful public credential handoff does not speculatively start a
    // second model worker. If Windows rejects the credential and sends another
    // AUTH_REQUEST, the ordinary fallback starts one then.
    MarkAuthWorkerConsumed(false);
    WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
    FACELOGIN_INFO(L"UserLoggedIn=1 written after auth success");
    m_pipeServer->DrainOutput(5000);
    return true;
}

bool FaceService::ProcessAuthRequest() {
    if (m_isServiceMode) return ProcessWorkerAuthRequest();

    FACELOGIN_INFO(L"Starting face authentication...");

    const ModelUseGuard modelUseGuard(*this);
    const ScopedPerformanceCoreAffinity affinityGuard(GetPerformanceCoreMask());

    bool modelsNeedWait = false;
    {
        std::lock_guard<std::mutex> lock(m_modelMutex);
        modelsNeedWait = m_modelState != ModelState::Ready;
    }
    if (modelsNeedWait) {
        m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) +
                                   L"正在加载模型...");
        FlushFileBuffers(m_pipeServer->GetHandle());
    }

    auto models = AcquireModelsForAuth();
    if (!models) {
        FACELOGIN_ERROR(L"Required models not loaded — cannot authenticate");
        const wchar_t* loadError = m_padIntegrityFailed.load()
            ? L"活体模型完整性校验失败，文件可能被篡改或损坏，请使用密码登录并重新安装 FaceLogin"
            : L"服务模型加载失败，请使用密码登录并检查模型文件";
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(loadError));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }
    if (!models->detector || !models->recognizer || !models->antiSpoof) {
        FACELOGIN_ERROR(L"Model lifecycle invariant violated — incomplete bundle published");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
            L"服务模型状态异常，请使用密码登录"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }
    if (m_livenessMethod != LivenessMethod::AntiSpoof ||
        !models->antiSpoof->IsInitialized()) {
        FACELOGIN_ERROR(L"Authentication refused: anti-spoof model is unavailable");
        const wchar_t* message = m_padIntegrityFailed.load()
            ? L"活体模型完整性校验失败，文件可能被篡改或损坏，请使用密码登录并重新安装 FaceLogin"
            : L"活体检测模块不可用，请使用密码登录并检查模型文件";
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(message));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }
    if (m_store->GetUserCount() == 0) {
        FACELOGIN_WARN(L"No registered users");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"没有注册用户"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    std::optional<CredentialStore::IdentityMatch> lockedIdentity;
    std::wstring initialSid;

    AuthPipelineCallbacks callbacks;
    callbacks.grabFrame = [this](FrameImage& frame) {
        return m_webcamMF && m_webcamMF->GrabFrame(frame);
    };
    callbacks.isCancelled = [this]() {
        return !m_running;
    };
    callbacks.isClientDisconnected = [this]() {
        return m_pipeServer->IsClientDisconnected();
    };
    callbacks.reportStatus = [this](const std::wstring& text) {
        m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + text);
        FlushFileBuffers(m_pipeServer->GetHandle());
    };
    callbacks.verifyBinding = [this, &lockedIdentity, &initialSid](
        const std::vector<float>& embedding, unsigned int bindingIndex) {
        auto identity = m_store->FindBestIdentity(embedding.data(), embedding.size(),
                                                  m_matchThreshold);
        if (!identity) {
            return BindingDecision{BindingDecisionKind::Retry, {}};
        }

        if (bindingIndex == 0) {
            if (lockedIdentity) {
                return BindingDecision{BindingDecisionKind::Reject,
                                       L"身份验证状态异常，请使用密码登录"};
            }
            if (identity->sid.empty()) {
                FACELOGIN_ERROR(L"Matched credential has an empty SID — enrollment data is invalid");
                return BindingDecision{BindingDecisionKind::Reject,
                                       L"身份数据无效，请使用密码登录并重新录入人脸"};
            }
            if (identity->passwordless) {
                FACELOGIN_WARN(L"Matched passwordless account '%s' — face login cannot unlock",
                               identity->username.c_str());
                return BindingDecision{BindingDecisionKind::Reject,
                                       ipc::MSG_PASSWORDLESS_NOTICE};
            }

            initialSid = identity->sid;
            lockedIdentity = std::move(identity);
            FACELOGIN_INFO(L"Identity locked: %s (distance=%.4f) [1/3]",
                           lockedIdentity->username.c_str(), lockedIdentity->distance);
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }

        if (!lockedIdentity || bindingIndex > 2 || identity->sid.empty() ||
            identity->sid != initialSid) {
            FACELOGIN_WARN(L"Identity changed during anti-spoof — rejecting face swap");
            return BindingDecision{BindingDecisionKind::Reject,
                                   L"活体验证期间人脸不匹配，请重试"};
        }

        FACELOGIN_INFO(L"Identity confirmed: %s [%u/3]", initialSid.c_str(),
                       bindingIndex + 1);
        return BindingDecision{BindingDecisionKind::Accept, {}};
    };

    AuthPipeline pipeline(
        *models->detector, *models->recognizer, *models->antiSpoof,
        AuthPipelineConfig{m_livenessMethod, m_antiSpoofThreshold,
                           m_authTimeoutSeconds, m_config.camera_rotation},
        std::move(callbacks));
    const AuthPipelineResult result = pipeline.Run();

    if (result.cancelled) {
        return false;
    }
    if (result.timedOut) {
        m_pipeServer->WriteMessage(ipc::MSG_AUTH_TIMEOUT);
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }
    if (!result.succeeded) {
        const std::wstring error = result.errorMessage.empty()
            ? L"认证过程异常，请使用密码登录"
            : result.errorMessage;
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(error));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }
    if (!lockedIdentity) {
        FACELOGIN_ERROR(L"Authentication reached final release without a locked identity");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
            L"身份验证状态异常，请使用密码登录"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    auto credential = m_store->LoadCredentialForSid(lockedIdentity->sid,
                                                     lockedIdentity->distance);
    if (!credential || credential->passwordless) {
        FACELOGIN_ERROR(L"Authorized identity could not provide a password credential");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
            credential && credential->passwordless
                ? ipc::MSG_PASSWORDLESS_NOTICE
                : L"账户凭据不可用，请使用密码登录"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        SecureClearMatchPassword(credential);
        return false;
    }

    std::wstring domain = L".";
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = ARRAYSIZE(computerName);
    if (GetComputerNameW(computerName, &size)) {
        domain = computerName;
    }

    std::wstring message = ipc::BuildAuthSuccessMessage(
        credential->sid, credential->upn, domain, credential->username,
        credential->password);
    const bool writeOk = m_pipeServer->WriteMessage(message);
    FlushFileBuffers(m_pipeServer->GetHandle());
    SecureClearWString(message);
    if (!writeOk) {
        FACELOGIN_WARN(L"Failed to send authentication credentials");
        SecureClearMatchPassword(credential);
        return false;
    }

    FACELOGIN_INFO(L"Credentials sent for %s\\%s", domain.c_str(),
                   credential->username.c_str());
    SecureClearMatchPassword(credential);
    WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
    FACELOGIN_INFO(L"UserLoggedIn=1 written after auth success");

    if (m_webcamMF) {
        m_webcamMF->Shutdown();
    }
    m_pipeServer->DrainOutput(5000);
    return true;
}

bool FaceService::Install(const std::wstring& exePath) {
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) {
        FACELOGIN_ERROR(L"OpenSCManager failed: %lu", GetLastError());
        return false;
    }

    SC_HANDLE hService = CreateServiceW(
        hSCManager, SERVICE_NAME, L"FaceLogin Authentication Service",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        exePath.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr);

    if (!hService) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_EXISTS) {
            FACELOGIN_ERROR(L"CreateService failed: %lu", err);
            CloseServiceHandle(hSCManager);
            return false;
        }
        hService = OpenServiceW(hSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
        if (!hService) {
            CloseServiceHandle(hSCManager);
            return false;
        }
    }

    SERVICE_DESCRIPTIONW desc = {};
    desc.lpDescription = const_cast<LPWSTR>(
        L"FaceLogin \u2014 custom face recognition authentication for Windows login");
    ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &desc);

    SERVICE_FAILURE_ACTIONSW fa = {};
    SC_ACTION actions[3] = {};
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 60000;
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = 60000;
    actions[2].Type = SC_ACTION_RESTART;
    actions[2].Delay = 60000;
    fa.dwResetPeriod = 86400;
    fa.lpRebootMsg = nullptr;
    fa.lpCommand = nullptr;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    FACELOGIN_INFO(L"Service installed: %s", exePath.c_str());
    return true;
}

bool FaceService::Uninstall() {
    SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) return false;

    SC_HANDLE hService = OpenServiceW(hSCManager, SERVICE_NAME, SERVICE_STOP | DELETE);
    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            CloseServiceHandle(hSCManager);
            return true;
        }
        CloseServiceHandle(hSCManager);
        return false;
    }

    SERVICE_STATUS status;
    ControlService(hService, SERVICE_CONTROL_STOP, &status);

    for (int i = 0; i < 30; i++) {
        QueryServiceStatus(hService, &status);
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(1000);
    }

    DeleteService(hService);
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);

    FACELOGIN_INFO(L"Service uninstalled");
    return true;
}

} // namespace facelogin
