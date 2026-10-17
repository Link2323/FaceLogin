#include "FaceService.h"
#include "auth_pipeline.h"
#include "../vision/face_align.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/data_path.h"
#include "../common/secure_clear.h"
#include "../common/text_util.h"
#include "../common/json_util.h"
#include "../common/embedding_math.h"
#include "../common/time_util.h"
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>
#include <thread>
#include <vector>
#include <wtsapi32.h>

#pragma comment(lib, "wtsapi32.lib")

namespace facelogin {

static constexpr wchar_t SERVICE_NAME[] = L"FaceLoginService";

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

FaceService::~FaceService() {
    m_residency.Stop();
    // Learner first: its Stop() flushes pending template updates through the
    // hooks, which take m_storeLock — both must still be alive here.
    if (m_learner) m_learner->Stop();
    if (m_storeLockReady) DeleteCriticalSection(&m_storeLock);
}

void WINAPI FaceService::ServiceMain(DWORD argc, LPWSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    FaceService service;

    service.m_hStatus = RegisterServiceCtrlHandlerExW(
        SERVICE_NAME, HandlerEx, &service);

    if (!service.m_hStatus) {
        FACELOGIN_ERROR(L"RegisterServiceCtrlHandler failed: %lu", GetLastError());
        return;
    }

    service.m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service.m_status.dwCurrentState = SERVICE_START_PENDING;
    service.m_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        | SERVICE_ACCEPT_SESSIONCHANGE;
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
                pService->m_residency.RequestUnload(L"session logon");
            } else if (eventType == WTS_SESSION_LOGOFF) {
                FACELOGIN_INFO(L"Session LOGOFF: session=%lu → UserLoggedIn=0",
                              evt->dwSessionId);
                // UserLoggedIn=0 drives the service's own model-preload
                // decision on the next logon (ShouldPreloadModels) — the
                // credential provider no longer reads it (cold-boot auto-
                // trigger removed 2026-08; recognition waits for input).
                WriteRegDword(REGVAL_USER_LOGGED_IN, 0);
                pService->m_residency.RequestLoad(L"session logoff");
            } else if (eventType == WTS_SESSION_LOCK) {
                // The queued model request logs itself ("Model load
                // requested: session lock") — no extra line here.
                pService->m_residency.RequestLoad(L"session lock");
            } else if (eventType == WTS_SESSION_UNLOCK) {
                pService->m_residency.RequestUnload(L"session unlock");
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

// Progressive-learning instrumentation: static norm of every stored template
// at load/reload, plus the slot's nominal pose (a format-V5 field; records
// without a pose target carry the invalid sentinel = "no cone"). Enrollment
// averages 5 unit-length samples
// without renormalizing, so the stored norm (<1) tracks how spread the
// enrollment frames were; changes over time reveal template drift. The
// nominal angle tells the field logs whether the pose-cone gate is armed for
// this slot.
static void LogTemplateNorms(const CredentialStore& store, const wchar_t* when) {
    for (const auto& u : store.GetUsers()) {
        for (const auto& f : u.faces) {
            const wchar_t* cone = IsValidNominalAngle(f.nominalYaw)
                ? L"cone on" : L"no cone";
            FACELOGIN_INFO(L"Template stats [%s]: user=%s face#%u(%s) dim=%zu "
                           L"norm=%.4f nominalYaw=%.0f nominalPitch=%.0f (%s)",
                           when, u.username.c_str(), f.id, f.label.c_str(),
                           f.embedding.size(), L2Norm(f.embedding),
                           f.nominalYaw, f.nominalPitch, cone);
        }
    }
}

// Persist template-learning updates. Runs only on the learner thread or at
// service shutdown. The first flush of a local day refreshes users.dat.bak —
// the rollback copy of last resort; failure only logs (learning is
// best-effort and must never block the write path).
void FaceService::FlushLearnedTemplates() {
    if (!m_store) return;
    const int today = LocalDayStamp();
    EnterCriticalSection(&m_storeLock);
    if (m_learningBakDay != today) {
        m_learningBakDay = today;
        const std::wstring src = m_dataDir + L"\\data\\users.dat";
        const std::wstring dst = src + L".bak";
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            FACELOGIN_WARN(L"users.dat.bak refresh failed (%lu) — updates still saved",
                           GetLastError());
        }
    }
    if (!m_store->SaveDatabase()) {
        FACELOGIN_ERROR(L"Template learning flush: SaveDatabase failed "
                        L"(updates retained in memory)");
    }
    LeaveCriticalSection(&m_storeLock);
}

// Learner handoff. Public-pipe thread only. The era distance windows live
// inside the learner (per-slot, keyed by the pose-selected target —
// 2026-09-05 revision); this side carries no learning statistics anymore.
void FaceService::SubmitLearningCandidate(const LearningSample& best) {
    if (!best.faceId) return;
    if (m_learner) {
        m_learner->Enqueue(best);
    }
}

// LEARNING_STATUS body. Compact single-message JSON (short keys, no pretty
// printing): the public pipe has no fragmentation, so the payload has to
// stay well inside the client's fixed read buffer.
std::wstring FaceService::BuildLearningStatusJson() {
    auto localTime = [](std::chrono::system_clock::time_point tp,
                        wchar_t* buf, size_t len) {
        const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
        std::tm localTm{};
        if (localtime_s(&localTm, &tt) != 0) {
            wcsncpy_s(buf, len, L"?", _TRUNCATE);
            return;
        }
        swprintf_s(buf, len, L"%04d-%02d-%02d %02d:%02d:%02d",
                   localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
                   localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
    };

    std::wostringstream ss;
    ss << L"{\"enabled\":" << (m_config.ema_learning ? L"true" : L"false")
       << L",\"gate_cap\":" << m_config.learning_distance_gate
       << L",\"alpha\":" << m_config.learning_alpha
       << L",\"interval\":" << m_config.learning_min_interval_sec;

    ss << L",\"faces\":[";
    bool first = true;
    if (m_learner) {
        for (const LearningFaceStatus& st : m_learner->FaceStatuses()) {
            if (!first) ss << L",";
            first = false;
            wchar_t last[24] = L"";
            if (st.hadAccept) localTime(st.lastAccept, last, 24);
            // Slot-level era window (2026-09-05): the p20 of THIS slot's
            // processed distances, or -1 while it holds fewer than 5 entries
            // (the gate then falls back to the account's pooled window).
            ss << L"{\"user\":\"" << JsonEscape(st.username) << L"\""
               << L",\"face\":" << st.faceId
               << L",\"label\":\"" << JsonEscape(st.faceLabel) << L"\""
               << L",\"total\":" << st.acceptedTotal
               << L",\"today\":" << st.acceptedToday
               << L",\"last\":\"" << last << L"\""
               << L",\"era_p20\":" << st.eraP20
               << L",\"era_rounds\":" << st.eraRounds
               << L"}";
        }
    }
    ss << L"]";

    ss << L",\"recent\":[";
    first = true;
    if (m_learner) {
        for (const LearningEvent& ev : m_learner->RecentEvents(24)) {
            if (!first) ss << L",";
            first = false;
            wchar_t t[24] = L"";
            localTime(ev.time, t, 24);
            ss << L"{\"t\":\"" << t << L"\""
               << L",\"user\":\"" << JsonEscape(ev.username) << L"\""
               << L",\"face\":" << ev.faceId
               << L",\"label\":\"" << JsonEscape(ev.faceLabel) << L"\""
               << L",\"ok\":" << (ev.accepted ? L"true" : L"false")
               << L",\"reason\":\"" << (ev.reason ? ev.reason : L"") << L"\""
               << L",\"d\":" << ev.distance
               << L",\"gate\":" << ev.gate;
            if (ev.accepted) ss << L",\"alpha\":" << ev.alphaUsed;
            ss << L"}";
        }
    }
    ss << L"]}";
    return ss.str();
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

    // Attach the log file before LoadConfig: LoadConfig logs the active
    // config (incl. the PAD threshold) exactly once per load, and calling it
    // before SetLogFile silently dropped the startup line — leaving the
    // pipeline's per-auth echo as the only record of the active threshold.
    std::wstring logPath = m_dataDir + L"\\log\\service.log";
    Logger::Instance().SetLogFile(logPath);
    Logger::Instance().SetMinLevel(LogLevel::Info);

    // Load configuration from config.json (defaults when the file is absent).
    // Must happen before camera init — the configured camera_device is used
    // below.
    m_config = LoadConfig(m_dataDir);
    m_matchThreshold = m_config.match_threshold;
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;

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
    LogTemplateNorms(*m_store, L"load");

    // Progressive template learning (docs/work/in-progress/progressive-learning-v2.md §3).
    // The learner thread owns all post-auth EMA work; the hooks below
    // serialize CredentialStore access under m_storeLock (single writer).
    InitializeCriticalSection(&m_storeLock);
    m_storeLockReady = true;
    {
        TemplateLearner::Hooks hooks;
        hooks.fetchTemplate = [this](const std::wstring& sid, uint32_t faceId,
                                     TemplateLearner::TemplateSnapshot& out) {
            EnterCriticalSection(&m_storeLock);
            const auto& users = m_store->GetUsers();
            for (const auto& u : users) {
                if (u.sid != sid) continue;
                for (const auto& f : u.faces) {
                    if (f.id == faceId) {
                        out.embedding = f.embedding;
                        out.nominalYaw = f.nominalYaw;
                        out.nominalPitch = f.nominalPitch;
                        LeaveCriticalSection(&m_storeLock);
                        return true;
                    }
                }
            }
            LeaveCriticalSection(&m_storeLock);
            return false;
        };
        hooks.commit = [this](const LearningSample& s, const std::vector<float>& blended,
                              float) {
            EnterCriticalSection(&m_storeLock);
            const bool ok = m_store->UpdateTemplateFace(s.sid, s.faceId, blended);
            LeaveCriticalSection(&m_storeLock);
            return ok;
        };
        hooks.flush = [this]() { FlushLearnedTemplates(); };
        LearningConfig learning;
        learning.enabled = m_config.ema_learning;
        learning.alpha = m_config.learning_alpha;
        learning.distanceGate = m_config.learning_distance_gate;
        learning.normFloor = m_config.learning_norm_floor;
        learning.minIntervalSec = m_config.learning_min_interval_sec;
        m_learner = std::make_unique<TemplateLearner>(learning, std::move(hooks));
    }

    m_pipeServer = std::make_unique<PipeServer>();

    // The parent owns no camera or ONNX session. Each worker handles one request.
    FACELOGIN_INFO(L"Authentication worker will initialize DirectShow on demand%s",
                   m_config.camera_device.empty() ? L"" : L" (configured device)");

    AuthenticationResidency::WorkerLoader loader = [this](const AppConfig& config, std::wstring& error) {
        return LoadAuthenticationWorker(config, error);
    };
    if (!m_residency.Start(m_config, std::move(loader))) {
        FACELOGIN_ERROR(L"Failed to start model lifecycle worker");
        return false;
    }

    const bool preload = ShouldPreloadModelsAtStartup();
    if (preload) {
        m_residency.RequestLoad(L"service startup / sign-in screen");
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

std::shared_ptr<AuthWorkerClient> FaceService::LoadAuthenticationWorker(
    const AppConfig& appConfig, std::wstring& errorMessage) {
    auth_worker::WorkerConfig workerConfig;
    // The production service remains in Session 0, where the worker always
    // uses the proven DirectShow path.
    workerConfig.cameraRotation = appConfig.camera_rotation;
    workerConfig.antiSpoofThreshold = appConfig.anti_spoof_threshold;
    workerConfig.authTimeoutSeconds = m_authTimeoutSeconds;
    workerConfig.cameraDevice = Utf8ToWstr(appConfig.camera_device);
    workerConfig.fastUnlock = appConfig.fast_unlock;

    auto worker = std::make_shared<AuthWorkerClient>(std::move(workerConfig));
    if (!worker->Start(errorMessage)) return {};
    return worker;
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
            bool reloaded = false;
            {
                EnterCriticalSection(&m_storeLock);
                reloaded = m_store->LoadDatabase();
                LeaveCriticalSection(&m_storeLock);
            }
            // Enrollment changed the templates: every era window describes
            // a gone era — reset (adaptive-gate era boundary,
            // docs/work/in-progress/progressive-learning-v2.md red line 1). Pacing and α
            // decay survive; only the windows clear.
            if (m_learner) m_learner->ResetEraWindows();
            if (reloaded) {
                SendControlResponse(ipc::MSG_RELOAD_OK);
                m_pipeServer->Disconnect();
                FACELOGIN_INFO(L"Database reloaded");
                LogTemplateNorms(*m_store, L"reload");
            } else {
                SendControlResponse(ipc::MSG_CONFIG_RELOAD_ERROR);
                m_pipeServer->Disconnect();
                FACELOGIN_ERROR(L"Database reload failed — users.dat parse error; "
                                L"previous in-memory database retained");
            }
        }
        else if (request == ipc::MSG_CONFIG_RELOAD) {
            AppConfig reloadedConfig = LoadConfig(m_dataDir);
            m_config = std::move(reloadedConfig);
            m_matchThreshold = m_config.match_threshold;
            m_antiSpoofThreshold = m_config.anti_spoof_threshold;
            if (m_learner) {
                LearningConfig learning;
                learning.enabled = m_config.ema_learning;
                learning.alpha = m_config.learning_alpha;
                learning.distanceGate = m_config.learning_distance_gate;
                learning.normFloor = m_config.learning_norm_floor;
                learning.minIntervalSec = m_config.learning_min_interval_sec;
                m_learner->UpdateConfig(learning);
            }

            const bool modelStateOk = m_residency.ReloadConfig(m_config);

            SendControlResponse(modelStateOk
                ? ipc::MSG_CONFIG_RELOAD_OK
                : ipc::MSG_CONFIG_RELOAD_ERROR);
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Configuration reloaded: thr=%.2f antiSpoof=%.3f rotation=%d",
                          m_matchThreshold, m_antiSpoofThreshold,
                          m_config.camera_rotation);
        }
        else if (request == ipc::MSG_LEARNING_STATUS) {
            // Read-only observability query from the console's learning
            // view. In-memory snapshot + era windows; both are owned by this
            // (public-pipe) thread, so no extra locking is needed beyond the
            // learner's own status critical section.
            SendControlResponse(BuildLearningStatusJson());
            m_pipeServer->Disconnect();
        }
        else if (request == ipc::MSG_AUTH_REQUEST) {
            ProcessAuthRequest();
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
    // Stop the learner before the store goes away: it flushes any pending
    // template updates. Enqueue is a no-op once stopped, so an in-flight
    // pipe round simply drops its sample; store access stays serialized by
    // m_storeLock either way.
    if (m_learner) m_learner->Stop();
    m_residency.Stop();
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

// ============================================================================
// Shared auth-session helpers (public pipe + identity binding)
// ============================================================================

BindingDecision FaceService::VerifyIdentityBinding(
    const std::vector<float>& embedding, unsigned int bindingIndex,
    float preNorm, float yawDeg, float pitchDeg,
    std::optional<CredentialStore::IdentityMatch>& lockedIdentity,
    std::wstring& initialSid, float* identityMissDistance,
    LearningSample* roundBest) const {
    float bestDistance = -1.0f;
    std::optional<CredentialStore::IdentityMatch> identity;
    {
        // The learner thread mutates templates concurrently; every store
        // access shares this lock (single-writer, shared-read).
        EnterCriticalSection(&m_storeLock);
        identity = m_store->FindBestIdentity(embedding.data(), embedding.size(),
                                             m_matchThreshold, &bestDistance);
        LeaveCriticalSection(&m_storeLock);
    }
    if (!identity) {
        // Track the closest miss of the round for the failure log: "0.81
        // against 0.80" (marginal capture conditions) is a different problem
        // from "0.95" (wrong face / broken enrollment).
        if (identityMissDistance && bestDistance >= 0.0f &&
            (*identityMissDistance < 0.0f || bestDistance < *identityMissDistance)) {
            *identityMissDistance = bestDistance;
        }
        return BindingDecision{BindingDecisionKind::Retry, {}};
    }

    if (roundBest &&
        (!roundBest->faceId || identity->distance < roundBest->distance)) {
        roundBest->sid = identity->sid;
        roundBest->username = identity->username;
        roundBest->faceId = identity->faceId;
        roundBest->faceLabel = identity->faceLabel;
        roundBest->embedding = embedding;
        roundBest->distance = identity->distance;
        roundBest->norm = preNorm;
        roundBest->probeYawDeg = yawDeg;
        roundBest->probePitchDeg = pitchDeg;
        // Landing clarity of THIS frame: how much the hit slot beat the
        // account's second-nearest face (negative secondFaceDistance = the
        // account has a single comparable face — no wrong-slot risk).
        roundBest->faceMargin = (identity->secondFaceDistance >= 0.0f)
            ? (identity->secondFaceDistance - identity->distance)
            : 1e9f;
        // Per-face pose/distance table for the learner's yaw-based target
        // selection. Store read — shares the store lock like every other
        // access (the learner thread may be committing concurrently).
        {
            EnterCriticalSection(&m_storeLock);
            roundBest->accountFaces = m_store->GetAccountFaceDistances(
                identity->sid, embedding.data(), embedding.size());
            LeaveCriticalSection(&m_storeLock);
        }
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

    // Binding evidence is "same SID as the locked frame"; the RID tail is
    // enough here, the full SID stays on the CP log's success line.
    const size_t lastDash = initialSid.rfind(L'-');
    const std::wstring sidTail = lastDash == std::wstring::npos
                                     ? initialSid
                                     : initialSid.substr(lastDash + 1);
    FACELOGIN_INFO(L"Identity confirmed: sid=…-%s [%u/3]", sidTail.c_str(),
                   bindingIndex + 1);
    return BindingDecision{BindingDecisionKind::Accept, {}};
}

bool FaceService::SendStatusMessage(const std::wstring& text) {
    // STATUS is advisory UI text. Message-mode pipes preserve ordering, and
    // the Credential Provider keeps an event-driven read pending, so forcing
    // FlushFileBuffers here only blocks the authentication supervisor until
    // LogonUI consumes a non-terminal update. Terminal messages use the
    // explicit bounded ACK contract below.
    return m_pipeServer->WriteMessage(
        std::wstring(ipc::MSG_STATUS_PREFIX) + text);
}

bool FaceService::SendTerminalMessage(const std::wstring& message) {
    return SendAcknowledgedMessage(message, ipc::MSG_AUTH_ACK);
}

bool FaceService::SendControlResponse(const std::wstring& message) {
    return SendAcknowledgedMessage(message, ipc::MSG_CONTROL_ACK);
}

bool FaceService::SendAcknowledgedMessage(const std::wstring& message,
                                          const wchar_t* expectedAck) {
    if (!m_pipeServer->WriteMessage(message)) return false;

    std::wstring ack;
    if (!m_pipeServer->ReadMessage(ack, ipc::PIPE_ACK_TIMEOUT_MS)) {
        FACELOGIN_WARN(L"Pipe response ACK not received within %lu ms",
                       ipc::PIPE_ACK_TIMEOUT_MS);
        return false;
    }
    const bool valid = ack == expectedAck;
    if (!valid) {
        FACELOGIN_WARN(L"Unexpected pipe response ACK");
    }
    SecureClearWString(ack);
    return valid;
}

bool FaceService::SendAuthErrorMessage(const std::wstring& message) {
    return SendTerminalMessage(ipc::BuildAuthErrorMessage(message));
}

bool FaceService::ProcessAuthRequest() {
    const auto authDeadline = ipc::MakeAuthDeadline();
    if (m_store->GetUserCount() == 0) {
        FACELOGIN_WARN(L"No registered users");
        SendAuthErrorMessage(L"没有注册用户");
        return false;
    }

    std::optional<CredentialStore::IdentityMatch> lockedIdentity;
    std::wstring initialSid;
    AuthWorkerResult workerResult;
    float identityMissDistance = -1.0f;
    LearningSample roundBest;   // best frame of the round, learning candidate
    // Outlives the residency-use scope below: the success path reaps the
    // worker only after AUTH_SUCCESS has been delivered to the CP.
    std::shared_ptr<AuthWorkerClient> worker;

    {
        // Prevent SESSION_UNLOCK from killing the worker between the public
        // request and its terminal response. The worker itself owns all model
        // and camera objects; this guard protects only the parent control
        // channel and lifecycle state.
        auto backendUse = m_residency.Acquire(authDeadline, [this] {
            SendStatusMessage(L"正在加载模型...");
        });
        worker = backendUse.worker;
        if (ipc::RemainingAuthTimeMs(authDeadline) == 0) {
            workerResult.outcome = AuthOutcome::TimedOut;
        } else if (!worker || !worker->IsReady()) {
            const auto& loadError = backendUse.error;
            FACELOGIN_ERROR(L"Authentication worker is not ready");
            workerResult.errorMessage = loadError.empty()
                ? L"认证工作进程加载失败，请使用密码登录"
                : loadError;
        } else {
            AuthWorkerCallbacks callbacks;
            callbacks.reportStatus = [this](const std::wstring& text) {
                SendStatusMessage(text);
            };
            callbacks.isCancelled = [this]() {
                return !m_running || m_pipeServer->IsClientDisconnected();
            };
            callbacks.verifyBinding = [this, &lockedIdentity, &initialSid,
                                       &identityMissDistance, &roundBest](
                const std::vector<float>& embedding, unsigned int bindingIndex,
                float preNorm, float yawDeg, float pitchDeg) {
                return VerifyIdentityBinding(embedding, bindingIndex, preNorm,
                                             yawDeg, pitchDeg,
                                             lockedIdentity, initialSid,
                                             &identityMissDistance, &roundBest);
            };
            workerResult = worker->Authenticate(std::move(callbacks), authDeadline);
        }
    }

    // Every worker has handled at most one request. The replacement decision
    // follows the complete parent-side result: a valid worker success can
    // still fail closed if the SID/credential disappeared before the single
    // final decrypt or if the public pipe breaks. In those cases preload a
    // fresh worker only when the lifecycle still says the desktop is locked.
    switch (workerResult.outcome) {
    case AuthOutcome::Cancelled:
        m_residency.WorkerConsumed(true);
        return false;
    case AuthOutcome::TimedOut:
        m_residency.WorkerConsumed(true);
        FACELOGIN_INFO(L"Authentication timed out in worker");
        SendTerminalMessage(ipc::MSG_AUTH_TIMEOUT);
        return false;
    case AuthOutcome::Failed:
    default: {
        m_residency.WorkerConsumed(true);
        const std::wstring message = workerResult.errorMessage.empty()
            ? L"认证工作进程执行失败，请使用密码登录"
            : workerResult.errorMessage;
        if (identityMissDistance >= 0.0f) {
            FACELOGIN_WARN(L"Authentication worker failed: %s (closest identity distance=%.3f, threshold=%.3f)",
                           message.c_str(), identityMissDistance, m_matchThreshold);
        } else {
            FACELOGIN_WARN(L"Authentication worker failed: %s", message.c_str());
        }
        SendAuthErrorMessage(message);
        return false;
    }
    case AuthOutcome::Succeeded:
        break;
    }
    if (!lockedIdentity) {
        m_residency.WorkerConsumed(true);
        FACELOGIN_ERROR(L"Worker reported success without a parent-bound identity");
        SendAuthErrorMessage(L"身份验证状态异常，请使用密码登录");
        return false;
    }

    if (ipc::RemainingAuthTimeMs(authDeadline) == 0) {
        m_residency.WorkerConsumed(true);
        SendTerminalMessage(ipc::MSG_AUTH_TIMEOUT);
        return false;
    }

    std::optional<CredentialStore::MatchResult> credential;
    {
        EnterCriticalSection(&m_storeLock);
        credential = m_store->LoadCredentialForSid(lockedIdentity->sid,
                                                    lockedIdentity->distance);
        LeaveCriticalSection(&m_storeLock);
    }
    if (!credential) {
        m_residency.WorkerConsumed(true);
        FACELOGIN_ERROR(L"Authorized identity could not provide a password credential");
        SendAuthErrorMessage(L"账户凭据不可用，请使用密码登录");
        return false;
    }
    if (ipc::RemainingAuthTimeMs(authDeadline) == 0) {
        SecureClearMatchPassword(credential);
        m_residency.WorkerConsumed(true);
        SendTerminalMessage(ipc::MSG_AUTH_TIMEOUT);
        return false;
    }

    std::wstring domain = L".";
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = ARRAYSIZE(computerName);
    if (GetComputerNameW(computerName, &size)) domain = computerName;

    std::wstring message = ipc::BuildAuthSuccessMessage(
        credential->sid, credential->upn, domain, credential->username,
        credential->password);
    // The CP acknowledges as soon as it has copied and scrubbed the transport
    // buffer, before entering LogonUI callbacks. This bounded handshake
    // replaces the former unbounded FlushFileBuffers call.
    const bool writeOk = SendTerminalMessage(message);
    SecureClearWString(message);
    if (!writeOk) {
        m_residency.WorkerConsumed(true);
        FACELOGIN_WARN(L"Failed to send authentication credentials");
        SecureClearMatchPassword(credential);
        return false;
    }

    // Learning candidate: a memory-only push; the learner thread waits out
    // the post-unlock busy window before any work (template_learner.h).
    SubmitLearningCandidate(roundBest);

    // The worker self-terminated when it sent AuthSucceeded; reclaim its Job
    // only now. CompleteTerminal used to run inside Authenticate(), placing
    // the process-teardown wait (0–2 ms dev machine, 15–40 ms slow machine)
    // between the match verdict and AUTH_SUCCESS. Delivery — the ACK above —
    // is the critical path; the reap is not.
    const auto reapStart = std::chrono::steady_clock::now();
    worker->Stop();
    workerResult.cleanupMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - reapStart).count();

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
    m_residency.WorkerConsumed(false);
    WriteRegDword(REGVAL_USER_LOGGED_IN, 1);
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
