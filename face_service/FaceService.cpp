#include "FaceService.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/frame_image.h"
#include "../common/sha256_util.h"
#include "../common/data_path.h"
#include "../common/secure_clear.h"
#include <shlobj.h>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <cmath>
#include <vector>
#include <wtsapi32.h>

#pragma comment(lib, "wtsapi32.lib")

namespace facelogin {

FaceService* FaceService::s_pInstance = nullptr;

static constexpr wchar_t SERVICE_NAME[] = L"FaceLoginService";

// Expected SHA-256 (lowercase hex) of each bundled v1.5 model file. The
// installer validates against the same values (see installer/FaceLoginSetup/
// internal/extract.go::requiredModels) at install time; the service re-verifies
// on every load so a model swapped on disk after install is rejected fail-closed
// (a tampered PAD model could otherwise always return real≥0.99 and defeat the
// bug3 photo-attack defense). These are the single source of truth for the C++
// runtime check; the Go manifest and tools/pad_calibration predate this and are
// not changed here.
static constexpr char kDetSha256[] =
    "07b62718eb454ee1881465c12d0d0546f2e916e3bb549f142dc221729bf7f4dc";
static constexpr char kRecognizerSha256[] =
    "b9b2ea32afaa88dfd226255f354ea241c3a744abf75b3dbdcf00c95f7f00e185";
static constexpr char kMiniFasV2Sha256[] =
    "b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907";
static constexpr char kMiniFasV1SeSha256[] =
    "ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676";

// UTF-8 → wide string, for passing config.camera_device to the camera backends.
static std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static void SecureClearMatchPassword(std::optional<CredentialStore::MatchResult>& match) {
    if (match) {
        SecureClearWString(match->password);
    }
}

// ============================================================================
// Hybrid (P+E core) affinity — auth-window optimization
// ============================================================================
//
// On P+E hybrid CPUs (e.g. i7-1360P: 4P+8E) the ONNX thread pool (8 threads =
// hardware_concurrency/2) spreads across P and E cores; every layer-reduction
// waits on the slowest E-core.  During an auth we pin the whole process to the
// P-cores (highest EfficiencyClass, including HT siblings) so every ONNX and
// MiniFAS thread runs on a fast core.  Uniform CPUs (max EfficiencyClass == 0,
// e.g. Ryzen 9 7945HX) are untouched — the mask is 0 and the guard is a no-op.

// Returns a mask of the highest-EfficiencyClass cores' logical processors,
// or 0 on uniform CPUs / failure.
static DWORD_PTR GetPerformanceCoreMask() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return 0;

    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
            &len)) {
        return 0;
    }

    BYTE maxEff = 0;
    for (size_t off = 0; off < buf.size();) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (info->Relationship == RelationProcessorCore) {
            maxEff = (std::max)(maxEff, info->Processor.EfficiencyClass);
        }
        off += info->Size;
        if (info->Size == 0) break;  // malformed — bail
    }
    if (maxEff == 0) return 0;  // uniform CPU — no P/E split

    DWORD_PTR mask = 0;
    for (size_t off = 0; off < buf.size();) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (info->Relationship == RelationProcessorCore &&
            info->Processor.EfficiencyClass == maxEff) {
            mask |= info->Processor.GroupMask[0].Mask;
        }
        off += info->Size;
        if (info->Size == 0) break;
    }
    return mask;
}

// RAII: pins the process to the P-core mask for the auth duration and restores
// the original mask on every exit path.
class ScopedPerformanceCoreAffinity {
public:
    explicit ScopedPerformanceCoreAffinity(DWORD_PTR mask) : m_active(mask != 0) {
        if (m_active) {
            GetProcessAffinityMask(GetCurrentProcess(), &m_oldMask, &m_systemMask);
            if (!SetProcessAffinityMask(GetCurrentProcess(), mask)) {
                FACELOGIN_WARN(L"SetProcessAffinityMask failed: %lu", GetLastError());
                m_active = false;
            } else {
                FACELOGIN_INFO(L"P-core affinity enabled (mask 0x%llX)", (unsigned long long)mask);
            }
        }
    }
    ~ScopedPerformanceCoreAffinity() {
        if (m_active) SetProcessAffinityMask(GetCurrentProcess(), m_oldMask);
    }
    ScopedPerformanceCoreAffinity(const ScopedPerformanceCoreAffinity&) = delete;
    ScopedPerformanceCoreAffinity& operator=(const ScopedPerformanceCoreAffinity&) = delete;
private:
    bool m_active;
    DWORD_PTR m_oldMask = 0;
    DWORD_PTR m_systemMask = 0;
};

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
        pService->Stop();
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT: {
        // PBT_APMRESUMESUSPEND = resumed from sleep/hibernation. The USB camera
        // may still be in low-power recovery, so force a fresh camera init on
        // the next auth instead of reusing a stale SourceReader.
        //
        // Consumed only on the MF (standalone) path: see the m_resumedFlag check
        // in ProcessAuthRequest's MF branch. The DS (service) path doesn't read
        // it — service mode rebuilds the DS camera on every auth anyway
        // (Shutdown+reset after each request), so a stale SourceReader can't
        // accumulate there. The flag is harmless when set but unconsumed.
        if (eventType == PBT_APMRESUMESUSPEND) {
            FACELOGIN_INFO(L"Power resume event — forcing camera re-init on next auth");
            pService->m_resumedFlag.store(true);
        }
        return NO_ERROR;
    }
    case SERVICE_CONTROL_SESSIONCHANGE: {
        // Only respond to LOGON (user signed in) and LOGOFF (user signed out).
        // Ignore other events like WTS_SESSION_LOCK (7), WTS_SESSION_UNLOCK (8),
        // WTS_CONSOLE_CONNECT (1), etc. — those don't change logged-in state.
        auto* evt = reinterpret_cast<WTSSESSION_NOTIFICATION*>(eventData);
        if (evt && evt->cbSize == sizeof(WTSSESSION_NOTIFICATION)
            && evt->dwSessionId == WTSGetActiveConsoleSessionId()) {
            if (eventType == WTS_SESSION_LOGON) {
                FACELOGIN_INFO(L"Session LOGON: session=%lu → UserLoggedIn=1",
                              evt->dwSessionId);
                WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
            } else if (eventType == WTS_SESSION_LOGOFF) {
                FACELOGIN_INFO(L"Session LOGOFF: session=%lu → UserLoggedIn=0",
                              evt->dwSessionId);
                WriteRegDword(REGVAL_USER_LOGGED_IN, 0);
                // Clear the service start uptime so the next logon is
                // detected as a cold boot (fresh ServiceStartUptime written
                // on next service restart, or if the service stays running,
                // the CP will see ServiceStartUptime=0 and treat it as cold
                // boot via the UserLoggedIn=0 fallback).
                WriteRegQword(REGVAL_SERVICE_START_UPTIME, 0);
            } else {
                FACELOGIN_INFO(L"Session change ignored: eventType=%lu", eventType);
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

    // Write the service's system uptime at startup for the CP's cold-boot
    // detection.  The CP compares its own uptime to this value:
    //   close to this value (within ~120s) → cold boot (CP loaded near service)
    //   far above, or below (cross-boot stale) → cold boot
    //   far above (same boot, hours later) → unlock
    //
    // ALWAYS overwrite — registry persists across reboots, and GetTickCount64
    // resets to 0 on each boot, so a stale value from a prior boot would
    // corrupt detection if we skipped the write.
    {
        ULONGLONG uptime = GetTickCount64();
        WriteRegQword(REGVAL_SERVICE_START_UPTIME, uptime);
        FACELOGIN_INFO(L"Initialize: ServiceStartUptime = %llu", uptime);
    }

    // Load SCRFD ONNX detector (gnkps variant — group-norm keypoints, the
    // rotation-fix family; provides the 5 alignment keypoints directly).
    // 10g tier: ~3.4x faster than 34g at -1% WIDER Face (96.17→95.19).
    //
    // Loaded SYNCHRONOUSLY because the pipe listener must be up as soon as
    // possible: SCRFD is needed for the very first frame of auth, and at
    // 15.5MB it loads in well under a second even on a cold disk. Everything
    // heavier (w600k_r50 44MB INT8 + dual MiniFAS) is deferred to a background
    // thread — see StartBackgroundModelLoad(). The lock screen therefore
    // connects to the pipe the moment it appears instead of waiting out the
    // model loads.
    m_onnxDetector = std::make_unique<OnnxDetector>();
    std::wstring onnxDetPath = m_modelsDir + L"\\det_10g_gnkps.onnx";
    if (!VerifyModelIntegrity(onnxDetPath, kDetSha256, L"SCRFD detector")) {
        FACELOGIN_ERROR(L"SCRFD detector integrity check failed — refusing to load "
                        L"(face detection unavailable). Reinstall FaceLogin or restore "
                        L"the original det_10g_gnkps.onnx.");
        return false;
    }
    if (m_onnxDetector->Initialize(onnxDetPath)) {
        FACELOGIN_INFO(L"SCRFD detector loaded");
    } else {
        FACELOGIN_ERROR(L"SCRFD detector failed to load — face detection unavailable");
        return false;
    }

    // Heavy models (w600k_r50 recognizer + dual MiniFAS anti-spoof) load in a
    // background thread so Initialize() can return and the pipe starts
    // listening immediately. ProcessAuthRequest() will wait for them via
    // EnsureModelsLoaded(). See StartBackgroundModelLoad().
    StartBackgroundModelLoad();
    if (m_isServiceMode) {
        // Camera is initialized lazily per auth request to avoid
        // device contention with the console app. See Run().
        FACELOGIN_INFO(L"DirectShow webcam will be initialized on demand%s",
                       m_config.camera_device.empty() ? L"" : L" (configured device)");
    } else {
        // Media Foundation camera is also initialized on demand — keeping
        // it open across auth sessions causes the source reader to stall
        // (especially when FaceLoginConsole is running concurrently).
        FACELOGIN_INFO(L"MF webcam will be initialized on demand%s",
                       m_config.camera_device.empty() ? L"" : L" (configured device)");
    }

    m_pipeServer = std::make_unique<PipeServer>();

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

    // NOTE: the "Liveness method: antispoof (ready=...)" log is emitted by
    // LoadHeavyModels() once the heavy models finish loading in the background
    // — at this point m_antiSpoof is still being constructed.
    FACELOGIN_INFO(L"Liveness method: antispoof (mandatory, fail-closed; heavy models loading in background)");
    FACELOGIN_INFO(L"Match threshold: %.3f", m_matchThreshold);
    FACELOGIN_INFO(L"Initialization complete (pipe listening; heavy models loading)");

    return true;
}

// ============================================================================
// Lazy model loading (cold-boot acceleration)
//
// The pipe listener must be up as soon as possible so the credential provider
// connects the moment the lock screen appears. Loading w600k_r50 (44MB INT8) +
// dual MiniFAS synchronously in Initialize() pushed that by seconds on a cold
// boot. Instead SCRFD (15.5MB) loads synchronously and the heavy models load
// here, in a background thread kicked off right before Run() enters the pipe
// loop. The lock screen shows several seconds after SCM starts the service,
// which is normally enough for the loads to finish — the user never waits.
// If a request arrives before they finish, ProcessAuthRequest() calls
// EnsureModelsLoaded(), which blocks until ready (or fails/stop).
// ============================================================================

bool FaceService::LoadHeavyModels(bool lowLightEnhance) {
    FACELOGIN_INFO(L"Loading heavy models in background...");

    // 1. InsightFace recognizer (w600k_r50.onnx, 44MB INT8 — the biggest load).
    {
        auto recognizer = std::make_unique<OnnxRecognizer>();
        std::wstring path = m_modelsDir + L"\\w600k_r50.onnx";
        if (!VerifyModelIntegrity(path, kRecognizerSha256, L"InsightFace w600k_r50 recognizer")) {
            FACELOGIN_ERROR(L"ONNX recognizer integrity check failed — recognition unavailable");
            return false;
        }
        if (!recognizer->Initialize(path)) {
            FACELOGIN_ERROR(L"ONNX recognizer failed to load — recognition unavailable");
            return false;
        }
        recognizer->SetLowLightEnhance(lowLightEnhance);
        std::lock_guard<std::mutex> lock(m_modelMutex);
        m_onnxRecognizer = std::move(recognizer);
    }
    FACELOGIN_INFO(L"ONNX recognizer loaded — using InsightFace w600k_r50");

    // 2. Anti-spoof (mandatory, fail-closed). Never fall back to identity-only
    // matching: a photo could otherwise release the stored credential.
    {
        auto antiSpoof = std::make_unique<OnnxAntiSpoof>();
        std::wstring miniFasV2Path = m_modelsDir + L"\\MiniFASNetV2.onnx";
        std::wstring miniFasV1SePath = m_modelsDir + L"\\MiniFASNetV1SE.onnx";
        if (!VerifyModelIntegrity(miniFasV2Path, kMiniFasV2Sha256, L"MiniFASNetV2 (PAD)") ||
            !VerifyModelIntegrity(miniFasV1SePath, kMiniFasV1SeSha256, L"MiniFASNetV1SE (PAD)")) {
            // A tampered PAD model is the most dangerous integrity failure — it
            // could always return real≥0.99 and defeat the bug3 photo-attack
            // defense. Stay fail-closed: do not call Initialize, leave
            // m_antiSpoof null, and let ProcessAuthRequest reject everything.
            FACELOGIN_ERROR(L"Anti-spoof model integrity check failed — authentication "
                            L"will remain disabled (fail-closed)");
            m_padIntegrityFailed.store(true);
            std::lock_guard<std::mutex> lock(m_modelMutex);
            m_antiSpoof.reset();
        } else if (!antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath)) {
            // Keep the pipe online so LogonUI receives a precise AUTH_ERROR
            // instead of treating a missing service as a transient problem.
            // ProcessAuthRequest remains fail-closed until CONFIG_RELOAD
            // restores a valid model.
            FACELOGIN_ERROR(L"Anti-spoof model unavailable — authentication will remain disabled");
            std::lock_guard<std::mutex> lock(m_modelMutex);
            m_antiSpoof.reset();
        } else {
            // Note: PAD stays on its calibrated raw-camera preprocessing path
            // (no low-light enhance) — only the recognizer uses that toggle.
            std::lock_guard<std::mutex> lock(m_modelMutex);
            m_antiSpoof = std::move(antiSpoof);
            FACELOGIN_INFO(L"Anti-spoof models loaded (MiniFASNetV2 + MiniFASNetV1SE)");
        }
    }

    FACELOGIN_INFO(L"Liveness method: antispoof (mandatory, fail-closed; ready=%s)",
                   (m_antiSpoof && m_antiSpoof->IsInitialized()) ? L"yes" : L"no");
    FACELOGIN_INFO(L"Heavy models loaded");
    return true;
}

void FaceService::StartBackgroundModelLoad() {
    // Capture the config value the loader needs NOW. The main thread can
    // rewrite m_config via CONFIG_RELOAD while the loader is running; reading
    // the struct here avoids a data race, and the loader's low-light toggle
    // is overridden by CONFIG_RELOAD afterward anyway.
    const bool lowLightEnhance = m_config.low_light_enhance;

    m_modelsLoading.store(true);
    m_modelLoadThread = std::thread([this, lowLightEnhance]() {
        // Scoped RAII so the flags are cleared and waiters released on every
        // exit path (including exceptions).
        struct LoadGuard {
            FaceService* svc;
            bool ok;
            ~LoadGuard() {
                svc->m_modelsLoading.store(false);
                svc->m_modelsReady.store(ok);
                svc->m_modelsFailed.store(!ok);
                svc->m_modelCv.notify_all();
            }
        };
        bool ok = false;
        try {
            ok = LoadHeavyModels(lowLightEnhance);
        } catch (const std::exception& e) {
            FACELOGIN_ERROR(L"Model loader threw: %hs", e.what());
        }
        LoadGuard guard{ this, ok };
    });
}

// Called from the main auth path before the first inference. Blocks until the
// heavy models are ready, fail, or the service is stopping. Returns false only
// if a REQUIRED model failed to load (auth cannot proceed) or service is stop.
bool FaceService::EnsureModelsLoaded() {
    if (m_modelsReady.load()) return true;
    if (m_modelsFailed.load() && !m_modelsLoading.load()) return false;

    std::unique_lock<std::mutex> lock(m_modelMutex);
    m_modelCv.wait(lock, [this]() {
        return m_modelsReady.load() || m_modelsFailed.load() || m_modelsAbort.load();
    });
    return m_modelsReady.load() && !m_modelsAbort.load();
}

// Release anyone blocked in EnsureModelsLoaded() during service shutdown so
// Stop() can join the loader thread without deadlocking.
void FaceService::AbortModelLoadWait() {
    m_modelsAbort.store(true);
    m_modelCv.notify_all();
    if (m_modelLoadThread.joinable()) {
        m_modelLoadThread.join();
    }
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
                                L"in-memory database may be empty until next successful reload");
            }
        }
        else if (request == ipc::MSG_CONFIG_RELOAD) {
            // Wait for the background model loader to finish so the pointer
            // reads/writes below don't race it. If loading failed, auth is
            // fail-closed anyway and CONFIG_RELOAD may restore the model.
            EnsureModelsLoaded();

            m_config = LoadConfig(m_dataDir);
            m_matchThreshold = m_config.match_threshold;
            m_livenessMethod = m_config.liveness_method;
            m_antiSpoofThreshold = m_config.anti_spoof_threshold;

            // dlib recognizer/detector were removed — recognition_model and
            // detector config values are ignored (pure ONNX now).
            if (m_livenessMethod == LivenessMethod::Blink) {
                FACELOGIN_WARN(L"liveness_method=blink no longer supported — using anti-spoof");
                m_livenessMethod = LivenessMethod::AntiSpoof;
            }
            if (m_livenessMethod != LivenessMethod::AntiSpoof) {
                FACELOGIN_WARN(L"CONFIG_RELOAD: unsupported liveness method rejected — enforcing anti-spoof");
                m_livenessMethod = LivenessMethod::AntiSpoof;
            }

            // Retry loading both anti-spoof models if either was unavailable.
            // All model-pointer mutations take m_modelMutex to stay consistent
            // with the background loader and any CONFIG_RELOAD-triggered reload.
            bool antiSpoofReady;
            {
                std::lock_guard<std::mutex> lock(m_modelMutex);
                if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
                    auto antiSpoof = std::make_unique<OnnxAntiSpoof>();
                    std::wstring miniFasV2Path = m_modelsDir + L"\\MiniFASNetV2.onnx";
                    std::wstring miniFasV1SePath = m_modelsDir + L"\\MiniFASNetV1SE.onnx";
                    // Re-verify integrity on CONFIG_RELOAD recovery too — a model
                    // swapped between service start and a reload attempt must not
                    // sneak in just because the original load failed.
                    if (!VerifyModelIntegrity(miniFasV2Path, kMiniFasV2Sha256, L"CONFIG_RELOAD MiniFASNetV2 (PAD)") ||
                        !VerifyModelIntegrity(miniFasV1SePath, kMiniFasV1SeSha256, L"CONFIG_RELOAD MiniFASNetV1SE (PAD)")) {
                        m_antiSpoof.reset();
                        m_padIntegrityFailed.store(true);
                        FACELOGIN_ERROR(L"CONFIG_RELOAD: anti-spoof integrity check failed — authentication remains fail-closed");
                    } else if (antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath)) {
                        m_antiSpoof = std::move(antiSpoof);
                        m_padIntegrityFailed.store(false);
                        FACELOGIN_INFO(L"CONFIG_RELOAD: dual MiniFAS PAD loaded successfully");
                    } else {
                        m_antiSpoof.reset();
                        m_padIntegrityFailed.store(false);
                        FACELOGIN_ERROR(L"CONFIG_RELOAD: anti-spoof unavailable — authentication remains fail-closed");
                    }
                }
                // Low-light enhancement is recognition-only. PAD stays on its
                // calibrated raw-camera preprocessing path.
                if (m_onnxRecognizer) {
                    m_onnxRecognizer->SetLowLightEnhance(m_config.low_light_enhance);
                }
                antiSpoofReady = m_antiSpoof && m_antiSpoof->IsInitialized();
            }
            m_pipeServer->WriteMessage(antiSpoofReady
                ? ipc::MSG_CONFIG_RELOAD_OK
                : ipc::MSG_CONFIG_RELOAD_ERROR);
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Configuration reloaded: rec=%hs det=%hs live=%hs thr=%.2f rotation=%d",
                          m_config.recognition_model.c_str(), m_config.detector.c_str(),
                          "antispoof",
                          m_matchThreshold, m_config.camera_rotation);
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
            // Lazy-init camera: create + start on demand, then fully shutdown
            // after auth to free the device for other processes.
            if (m_isServiceMode) {
                if (!m_webcamDS) {
                    m_webcamDS = std::make_unique<WebcamCaptureDS>();
                    if (!m_webcamDS->Initialize(640, 480, Utf8ToWstr(m_config.camera_device))) {
                        FACELOGIN_ERROR(L"DS camera init failed on demand");
                        m_webcamDS.reset();
                        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"摄像头不可用"));
                        FlushFileBuffers(m_pipeServer->GetHandle());
                        m_pipeServer->DrainOutput(5000);
                        m_pipeServer->Disconnect();
                        continue;
                    }
                    FACELOGIN_INFO(L"DS camera initialized on demand for auth");
                }
            } else {
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
            if (m_isServiceMode && m_webcamDS) {
                m_webcamDS->Shutdown();
                m_webcamDS.reset();
                FACELOGIN_INFO(L"DS camera released after auth");
            }
            if (!m_isServiceMode && m_webcamMF) {
                m_webcamMF->Shutdown();
                m_webcamMF.reset();
                FACELOGIN_INFO(L"MF camera released after auth");
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
    // If the heavy models are still loading in the background (fast stop right
    // after start), release anyone blocked in EnsureModelsLoaded() and join the
    // loader thread before tearing down the rest — otherwise the loader could
    // write to m_onnxRecognizer/m_antiSpoof after they're destroyed.
    AbortModelLoadWait();
    if (m_isServiceMode && m_webcamDS) {
        m_webcamDS->Shutdown();
        m_webcamDS.reset();
    }
    if (!m_isServiceMode && m_webcamMF) {
        m_webcamMF->Shutdown();
    }
    if (m_pipeServer) {
        m_pipeServer->Close();
    }
}

bool FaceService::ProcessAuthRequest() {
    FACELOGIN_INFO(L"Starting face authentication...");

    // Pin ONNX/MiniFAS threads to the P-cores for the whole auth on hybrid
    // CPUs (no-op elsewhere).  Restored on every exit path by the guard.
    const ScopedPerformanceCoreAffinity affinityGuard(GetPerformanceCoreMask());

    // Heavy models (w600k_r50 + dual MiniFAS) load in the background during
    // startup. Normally ready by the time the user triggers auth; if the lock
    // screen appeared unusually fast (cold boot), block here until they finish
    // rather than failing. The auth timeout is running from when the CP
    // connected, so this only ever costs the tail of the boot time.
    if (m_modelsLoading.load() && !m_modelsReady.load()) {
        m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) +
                                   L"\u6b63\u5728\u52a0\u8f7d\u6a21\u578b...");
    }
    if (!EnsureModelsLoaded()) {
        FACELOGIN_ERROR(L"Required models not loaded — cannot authenticate");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"\u670d\u52a1\u6a21\u578b\u52a0\u8f7d\u5931\u8d25"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        m_pipeServer->Disconnect();
        return false;
    }

    // Authentication must never degrade to identity-only matching.  This
    // check also covers a model that was removed/corrupted after startup and a
    // failed hot-reload attempt.
    if (m_livenessMethod != LivenessMethod::AntiSpoof ||
        !m_antiSpoof || !m_antiSpoof->IsInitialized()) {
        FACELOGIN_ERROR(L"Authentication refused: anti-spoof model is unavailable");
        // Distinguish an integrity (tamper) failure from a generic load failure
        // so the cause is visible on the lock screen, not just in the log.
        const wchar_t* msg = m_padIntegrityFailed.load()
            ? L"活体模型完整性校验失败，文件可能被篡改或损坏，请使用密码登录并重新安装 FaceLogin"
            : L"活体检测模块不可用，请使用密码登录并检查模型文件";
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(msg));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    // Single grab helper used by ALL auth stages (match, anti-spoof, blink,
    // final verify): it applies the configured camera rotation so every stage
    // operates on identically-oriented frames. Previously rotation was only
    // applied in the match loop, leaving the liveness/verify stages to process
    // unrotated frames — with 90/270 rotation the face was sideways there and
    // detection/landmarks/EAR failed, blocking unlock.
    auto grabFrame = [this](FrameImage& f) -> bool {
        bool ok = m_isServiceMode ? m_webcamDS->GrabFrame(f)
                                  : (m_webcamMF ? m_webcamMF->GrabFrame(f) : false);
        if (ok) RotateFrame(f, m_config.camera_rotation);
        return ok;
    };

    if (m_store->GetUserCount() == 0) {
        FACELOGIN_WARN(L"No registered users");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(L"\u6ca1\u6709\u6ce8\u518c\u7528\u6237"));
        FlushFileBuffers(m_pipeServer->GetHandle());
        m_pipeServer->DrainOutput(5000);
        return false;
    }

    // Drop initial frames to let camera exposure adjust. Exposure settles
    // within 3 frames in practice (measured: match distance is stable from the
    // first kept frame); 5 frames × 50ms was over-conservative. 3 frames × 20ms
    // saves ~0.2s per auth with no accuracy regression (see
    // docs/performance-baseline.md experiment 3).
    FrameImage frame;
    for (int i = 0; i < 3; i++) {
        grabFrame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // No separate "识别中..." status push here — the credential provider's
    // StartAuth already pushed it on connect, and the fused loop below pushes
    // "正在识别..." as soon as it begins. A status between them would just
    // cause a brief text flash on the tile.

    auto startTime = std::chrono::steady_clock::now();
    bool authSent = false;
    // The first anti-spoof anchor frame (below) locks the identity for this
    // auth; every later embedding (mid-window consensus frame, final anchor)
    // must return this same SID or authentication fails closed.  The final
    // anchor (PAD frame 5) is the last identity gate — there is no separate
    // post-liveness verify (see the tail-anchor note further down).
    // lockedMatch carries the credential released on success.
    std::wstring initialSid;
    std::optional<CredentialStore::MatchResult> lockedMatch;

    std::wstring domain = L".";
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = ARRAYSIZE(computerName);
    if (GetComputerNameW(computerName, &size)) {
        domain = computerName;
    }

    // === Consensus + liveness, fused into a single window ===
    // Historically auth ran a separate 3-frame match consensus (3 embeddings)
    // ahead of a 5-frame PAD stage that re-anchored identity on frames 1/5
    // (2 more embeddings): 6 embeddings per auth, and the consensus frames
    // had NO PAD coverage.  This fused loop runs PAD on every frame (5/5 must
    // pass — unchanged) and binds identity on frames 1/3/5 of the PAD window:
    // the first anchor locks the SID, the mid-window embedding and the final
    // anchor must return the same SID.  The "3-frame consensus" strength is
    // preserved with three spaced samples (harder for a transient lighting
    // glitch to hit than three consecutive frames) at 3 embeddings per auth —
    // the final anchor doubles as the post-liveness identity gate, so no
    // separate fresh-frame final verify runs (tail-anchor reuse, reviewed
    // 2026-08: the old verify closed only the ~200ms window between frame 5
    // and credential release at the cost of a full extra embedding).
    // An embedding that fails to produce a match slides the frame out of the
    // count — it is neither counted for PAD nor allowed to satisfy the anchor
    // schedule, so consensus can never silently shrink below three bindings.
    // An embedding that matches a DIFFERENT SID is a hard fail (face swap).
        {
            // === Liveness check ===
            {
                LivenessMethod method = m_livenessMethod;

                // Determine status text. The fused loop runs face detection,
                // liveness, and identity binding together on each frame — show
                // one unified "recognizing" status rather than implying a
                // standalone "liveness-only" phase.
                m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + L"正在识别...");
                FlushFileBuffers(m_pipeServer->GetHandle());

                bool livenessPassed = false;
                bool livenessInferenceError = false;
                bool livenessIdentityMismatch = false;
                bool anyFaceSeen = false; // any frame detected a face this window?

                if (method == LivenessMethod::AntiSpoof) {
                    // Calibrated dual-model consensus: all five frames must pass.
                    int totalChecks = AntiSpoofCheckCount(m_antiSpoofThreshold);
                    int passRequired = AntiSpoofPassRequired(totalChecks);
                    FACELOGIN_INFO(L"Anti-spoof: threshold=%.3f → %d checks, %d required",
                                   m_antiSpoofThreshold, totalChecks, passRequired);
                    auto asStart = std::chrono::steady_clock::now();
                    int passCount = 0, totalChecked = 0;
                    // Anchor schedule inside the PAD window (see the fused
                    // design note above): frames 1/3/5 of the counted PAD
                    // samples embed and must agree on one SID; frames 2/4 use
                    // a cheap bbox-overlap continuity check.  A full embedding
                    // on every frame costs ~1-2s on low-end hardware and blew
                    // the old 5s window there; the 1/3/5 schedule keeps three
                    // bindings while staying inside the 8s window.
                    bool anchored = false;    // first anchor landed (identity locked)?
                    bool havePrevRect = false;
                    FaceRect prevRect; // last counted frame's face box
                    int consensusCount = 0;   // anchor + mid + final = 3 bindings
                    // Fail-fast timers (wall-clock, machine-independent). The
                    // inter-frame pacing (30ms retry on no-face / no-grab,
                    // 60ms between counted PAD frames) makes a frame-count
                    // cutoff machine-dependent: on a throttled 2GHz laptop a
                    // frame is ~600-1000ms, so "N frames" lasts several times
                    // longer there than on the dev Ryzen.  Wall-clock seconds
                    // give the same perceived wait on every machine and only
                    // ever shorten the failure path — the success path's frame
                    // schedule and the 8s/15s windows are untouched.
                    //   noFaceStart: when the current no-face streak began.
                    //   noPassStart: when the current all-PAD-fail streak began.
                    // Both reset to "now" when their condition clears, and both
                    // start at the window start.
                    auto noFaceStart = asStart;
                    auto noPassStart = asStart;
                    // Fail-fast triggers (set below, consulted after the loop).
                    // When either fires we break out and skip the normal
                    // livenessPassed computation so the false verdict sticks.
                    bool failFastEmpty = false;   // empty scene ≥ 2.5s
                    bool failFastAttack = false;  // face seen, PAD all-fail ≥ 2s
                    while (m_running && totalChecked < totalChecks) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
                            SecureClearMatchPassword(lockedMatch);
                            return false;
                        }
                        // Global 15s auth timeout.  The separate consensus loop
                        // that used to enforce it is gone; this loop owns it.
                        auto allElapsed = std::chrono::steady_clock::now() - startTime;
                        if (std::chrono::duration_cast<std::chrono::seconds>(allElapsed).count() >= m_authTimeoutSeconds) {
                            FACELOGIN_INFO(L"Authentication timed out");
                            m_pipeServer->WriteMessage(ipc::MSG_AUTH_TIMEOUT);
                            FlushFileBuffers(m_pipeServer->GetHandle());
                            m_pipeServer->DrainOutput(5000);
                            SecureClearMatchPassword(lockedMatch);
                            return false;
                        }
                        // Window matches enrollment's 8s (EnrollmentWizard).
                        auto asElapsed = std::chrono::steady_clock::now() - asStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(asElapsed).count() >= 8) break;

                        // Fail-fast: empty scene. If no face has been detected
                        // yet this window and 2.5s have elapsed, the scene is
                        // empty — bail now with "未检测到人脸" instead of
                        // making the user wait the full 8s window. 2.5s
                        // tolerates slow camera exposure / weak-light first-
                        // frame delay (anyFaceSeen flips true the instant a
                        // face appears, which stops this check for the rest of
                        // the window). Wall-clock so the wait is the same on a
                        // 2GHz throttled laptop as on the dev Ryzen.
                        if (!anyFaceSeen) {
                            auto noFaceElapsed = std::chrono::steady_clock::now() - noFaceStart;
                            if (std::chrono::duration<double>(noFaceElapsed).count() >= 2.5) {
                                FACELOGIN_INFO(L"No face detected within 2.5s — failing fast (empty scene)");
                                failFastEmpty = true;
                                // anyFaceSeen stays false -> "未检测到人脸" below
                                break;
                            }
                        }

                        FrameImage asFrame;
                        if (!grabFrame(asFrame)) { if (!m_running) break; std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

                        // MiniFASNet consumes expanded crops around the SCRFD bbox.
                        auto asDet = m_onnxDetector->DetectLargestFace(asFrame);
                        if (!asDet) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }
                        anyFaceSeen = true;

                        // Fail-fast: persistent PAD rejection. A face is
                        // present but passCount is still 0 (no frame has
                        // cleared the threshold) and 2s have elapsed since the
                        // first detection — the subject is almost certainly a
                        // photo/mask, so fail now instead of running the
                        // remaining PAD frames. The 2s grace window lets a
                        // real face that needs a frame or two of exposure
                        // settle clear the threshold first; once any frame
                        // passes, noPassStart is reset below and this check
                        // stays dormant for the rest of the window.
                        if (passCount == 0) {
                            auto noPassElapsed = std::chrono::steady_clock::now() - noPassStart;
                            if (std::chrono::duration<double>(noPassElapsed).count() >= 2.0) {
                                FACELOGIN_INFO(L"PAD persistently below threshold for 2s — failing fast (likely attack)");
                                failFastAttack = true;
                                // anyFaceSeen is true -> "未通过活体检测" below
                                break;
                            }
                        }

                        const FaceRect faceRect(
                            static_cast<long>(asDet->x1), static_cast<long>(asDet->y1),
                            static_cast<long>(asDet->x2), static_cast<long>(asDet->y2));

                        // Frame 1 (until the first anchor lands) and frame 5
                        // (totalChecks-1) are identity anchors; frame 3
                        // (totalChecked==2) is the mid-window consensus embed.
                        // The 1/3/5 schedule assumes totalChecks == 5 (the
                        // calibrated anti_spoof_threshold=0.281 default).  If a
                        // future config ever changes the check count, the
                        // binding plan must be re-derived so that exactly three
                        // spaced embeddings still bracket the window.
                        const bool isAnchor = !anchored || totalChecked == totalChecks - 1;
                        const bool isConsensusFrame = (totalChecked == 2);

                        // On binding frames the PAD inference runs concurrently
                        // with the embedding (independent ONNX sessions, read-only
                        // input — same rationale as the dual-MiniFAS overlap in
                        // OnnxAntiSpoof::Predict). Middle frames run it alone.
                        auto scoreFuture = std::async(std::launch::async,
                            [&] { return m_antiSpoof->Predict(asFrame, faceRect); });

                        if (isAnchor || isConsensusFrame) {
                            auto continuityEmb =
                                m_onnxRecognizer->ComputeEmbedding(asFrame, asDet->kps);
                            if (continuityEmb.empty()) {
                                scoreFuture.get(); // consume the running predict
                                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                continue;
                            }
                            auto continuityMatch = m_store->FindBestMatch(
                                continuityEmb.data(), continuityEmb.size(), m_matchThreshold);
                            if (!continuityMatch) {
                                scoreFuture.get();
                                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                continue;
                            }

                            if (!anchored) {
                                // First anchor: lock the identity.  This is the
                                // credential released on success — never clear
                                // its password here.
                                if (continuityMatch->sid.empty()) {
                                    scoreFuture.get();
                                    FACELOGIN_ERROR(L"Matched credential has an empty SID — enrollment data is invalid");
                                    SecureClearMatchPassword(continuityMatch);
                                    m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
                                        L"身份数据无效，请使用密码登录并重新录入人脸"));
                                    FlushFileBuffers(m_pipeServer->GetHandle());
                                    m_pipeServer->DrainOutput(5000);
                                    SecureClearMatchPassword(lockedMatch);
                                    return false;
                                }
                                if (continuityMatch->passwordless) {
                                    scoreFuture.get();
                                    FACELOGIN_WARN(L"Matched passwordless account '%s' — face login cannot unlock; notifying CP",
                                                   continuityMatch->username.c_str());
                                    SecureClearMatchPassword(continuityMatch);
                                    m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(ipc::MSG_PASSWORDLESS_NOTICE));
                                    FlushFileBuffers(m_pipeServer->GetHandle());
                                    m_pipeServer->DrainOutput(5000);
                                    SecureClearMatchPassword(lockedMatch);
                                    return false;
                                }
                                initialSid = continuityMatch->sid;
                                lockedMatch = std::move(continuityMatch);
                                anchored = true;
                                consensusCount++;
                                FACELOGIN_INFO(L"Identity locked: %s (distance=%.4f) [%d/3]",
                                              lockedMatch->username.c_str(),
                                              lockedMatch->distance, consensusCount);
                            } else {
                                // Mid-window consensus / final anchor: must be
                                // the same identity that locked the sequence.
                                // Otherwise user A's stored credential could be
                                // released after user B (or a swapped face)
                                // supplied the liveness frames.
                                const bool sameIdentity = !continuityMatch->sid.empty() &&
                                                          continuityMatch->sid == initialSid;
                                SecureClearMatchPassword(continuityMatch);
                                if (!sameIdentity) {
                                    scoreFuture.get();
                                    FACELOGIN_WARN(L"Identity changed during anti-spoof — rejecting face swap");
                                    livenessIdentityMismatch = true;
                                    break;
                                }
                                consensusCount++;
                                FACELOGIN_INFO(L"Identity confirmed: %s [%d/3]",
                                              initialSid.c_str(), consensusCount);
                            }
                        } else if (havePrevRect) {
                            // Middle frames: cheap geometric continuity. A real
                            // face swap displaces the box beyond the ~35% IoU
                            // band; frame 5 re-binds identity by embedding. An
                            // IoU dip just skips the frame (forgiving of sway);
                            // a sustained displacement starves totalChecked and
                            // the run fails closed.
                            const auto interW = std::max<long>(0,
                                std::min(faceRect.right(), prevRect.right()) -
                                std::max(faceRect.left(), prevRect.left()) + 1);
                            const auto interH = std::max<long>(0,
                                std::min(faceRect.bottom(), prevRect.bottom()) -
                                std::max(faceRect.top(), prevRect.top()) + 1);
                            const double inter = static_cast<double>(interW) * interH;
                            const double uni = static_cast<double>(faceRect.width()) * faceRect.height() +
                                               static_cast<double>(prevRect.width()) * prevRect.height() - inter;
                            if (uni > 0.0 && inter / uni < 0.35) {
                                scoreFuture.get();
                                FACELOGIN_WARN(L"Face position discontinuity during anti-spoof — frame skipped");
                                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                continue;
                            }
                        }

                        float score = scoreFuture.get();
                        if (!std::isfinite(score) || score < 0.0f || score > 1.0f) {
                            FACELOGIN_ERROR(L"Anti-spoof inference returned invalid score: %.4f", score);
                            livenessInferenceError = true;
                            break;
                        }

                        totalChecked++;
                        prevRect = faceRect;
                        havePrevRect = true;
                        if (score >= m_antiSpoofThreshold) {
                            passCount++; // config-driven threshold
                            // A frame cleared the threshold — the persistent-
                            // rejection streak is broken; reset the fail-fast
                            // timer so a later run of failures must again
                            // exceed 2s before early-exiting.
                            noPassStart = std::chrono::steady_clock::now();
                        }
                        FACELOGIN_INFO(L"Anti-spoof frame %d: score=%.3f (pass=%d)", totalChecked, score, passCount);

                        // Inter-frame pacing for temporal diversity (distinct
                        // frames resist video-replay attacks). 60ms keeps the
                        // samples spread without the over-conservative 100ms
                        // (see docs/performance-baseline.md experiment 3).
                        std::this_thread::sleep_for(std::chrono::milliseconds(60));
                    }
                    // A partial sample set is not enough.  Previously one early
                    // passing frame could satisfy passRequired even when the
                    // remaining required frames were never captured.  The
                    // consensusCount >= 3 term is defensive: the loop's binding
                    // schedule already forces all three embeddings to succeed
                    // before totalChecked can reach totalChecks, but the check
                    // makes it impossible for a future edit to silently weaken
                    // consensus below three same-SID bindings.
                    // Fail-fast breaks leave livenessPassed=false (its init);
                    // skip the normal computation so the verdict sticks.
                    if (!failFastEmpty && !failFastAttack) {
                        livenessPassed = (!livenessInferenceError &&
                                          totalChecked == totalChecks &&
                                          passCount >= passRequired &&
                                          consensusCount >= 3);
                    }
                    if (!livenessPassed) {
                        FACELOGIN_WARN(L"Anti-spoof check failed: %d/%d passed (need %d)",
                                       passCount, totalChecked, passRequired);
                    }
                }

                if (!livenessPassed) {
                    FACELOGIN_WARN(L"Liveness check failed");
                    // Distinguish "never detected a face this window" from a
                    // genuine anti-spoof rejection.  Previously an empty scene
                    // (5/5 frames with no detection) timed out the 8s window
                    // with all three flags false and fell through to the
                    // attack-rejection message — telling the user "检测到攻击"
                    // when in fact no face was ever seen.
                    std::wstring failMsg;
                    if (livenessInferenceError) {
                        failMsg = L"活体检测模块异常，请使用密码登录";
                    } else if (livenessIdentityMismatch) {
                        failMsg = L"活体验证期间人脸不匹配，请重试";
                    } else if (!anyFaceSeen) {
                        failMsg = L"未检测到人脸";
                    } else {
                        failMsg = L"未通过活体检测，请使用真实人脸";
                    }
                    m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(failMsg));
                    FlushFileBuffers(m_pipeServer->GetHandle());
                    m_pipeServer->DrainOutput(5000);
                    SecureClearMatchPassword(lockedMatch);
                    return false;
                }

                // The final anchor (PAD frame 5) doubles as the post-liveness
                // identity check: it is the last PAD sample AND its embedding
                // already proved the same SID as the first anchor.  A separate
                // fresh-frame final verify existed solely to close the ~200ms
                // window between frame 5 and credential release - it cost a
                // whole extra embedding (~0.7s on slow hardware).  After
                // review the window is accepted and that step removed: the
                // tail-anchor binding is the final identity gate (see
                // docs/performance-baseline.md experiment 7).  Credentials go
                // out immediately after the 5/5 PAD pass.
                FACELOGIN_INFO(L"Liveness passed \u2014 tail anchor bound, releasing credentials");

            }

            // Serialize credentials only after liveness 5/5 and the tail-anchor
            // same-SID binding have both succeeded.
            std::wstring msg = ipc::BuildAuthSuccessMessage(
                lockedMatch->sid, lockedMatch->upn,
                domain, lockedMatch->username, lockedMatch->password);
            bool writeOk = m_pipeServer->WriteMessage(msg);
            FlushFileBuffers(m_pipeServer->GetHandle());
            SecureClearWString(msg);
            SecureClearMatchPassword(lockedMatch);

            if (!writeOk) {
                FACELOGIN_WARN(L"Failed to send authentication credentials");
                return false;
            }

            authSent = true;
            FACELOGIN_INFO(L"Credentials sent for %s\\%s",
                          domain.c_str(), lockedMatch->username.c_str());

            // Mark user as logged in IMMEDIATELY after sending credentials.
            // This prevents a race condition: the user can lock (Win+L)
            // before Windows fires WTS_SESSION_LOGON (which can take
            // seconds), causing the CP to misdetect the unlock as a cold
            // boot because UserLoggedIn is still 0.
            WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
            FACELOGIN_INFO(L"UserLoggedIn=1 written after auth success");

            // Stop the capture graph NOW (camera LED off) before the bounded
            // pipe drain below. The graph keeps streaming during auth; pausing
            // it immediately after success frees the camera without waiting for
            // the full teardown.
            if (m_isServiceMode && m_webcamDS) {
                m_webcamDS->Pause();
            } else if (!m_isServiceMode && m_webcamMF) {
                m_webcamMF->Shutdown();
            }

            // Bounded drain: wait briefly for the client to consume the
            // AUTH_SUCCESS message, then return. The old code did an
            // unbounded ReadFile(dummy) here — if the client closed the pipe
            // or never read, the service blocked forever and SCM killed it.
            m_pipeServer->DrainOutput(5000);
        }

    return authSent;
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
