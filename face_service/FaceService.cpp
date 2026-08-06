#include "FaceService.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/image_utils.h"
#include <shlobj.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cmath>
#include <wtsapi32.h>

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

static void SecureClearMatchPassword(std::optional<CredentialStore::MatchResult>& match) {
    if (match && !match->password.empty()) {
        SecureZeroMemory(match->password.data(), match->password.size() * sizeof(wchar_t));
        match->password.clear();
    }
}

static void SecureClearWideString(std::wstring& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
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

    service.m_status.dwCurrentState = SERVICE_STOPPED;
    service.m_status.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(service.m_hStatus, &service.m_status);
    FACELOGIN_INFO(L"FaceLoginService stopped");
}

void FaceService::RunStandalone() {
    FaceService service;

    {
        std::wstring logDir = ReadRegString(REGVAL_DATA_PATH, L"");
        if (logDir.empty()) {
            wchar_t programData[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
                logDir = std::wstring(programData) + L"\\FaceLogin";
            } else {
                logDir = L"C:\\ProgramData\\FaceLogin";
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
    {
        std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
        if (!regData.empty()) {
            m_dataDir = regData;
        } else {
            wchar_t programData[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
                m_dataDir = std::wstring(programData) + L"\\FaceLogin";
            } else {
                m_dataDir = L"C:\\ProgramData\\FaceLogin";
            }
        }
    }
    CreateDirectoryW(m_dataDir.c_str(), nullptr);
    m_modelsDir = GetModelsDir();

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
    m_onnxDetector = std::make_unique<OnnxDetector>();
    std::wstring onnxDetPath = m_modelsDir + L"\\det_10g_gnkps.onnx";
    if (m_onnxDetector->Initialize(onnxDetPath)) {
        FACELOGIN_INFO(L"SCRFD detector loaded");
    } else {
        FACELOGIN_ERROR(L"SCRFD detector failed to load — face detection unavailable");
        return false;
    }

    // Try loading InsightFace ONNX model (the only recognizer).
    m_onnxRecognizer = std::make_unique<OnnxRecognizer>();
    std::wstring onnxRecPath = m_modelsDir + L"\\w600k_r50.onnx";
    if (m_onnxRecognizer->Initialize(onnxRecPath)) {
        FACELOGIN_INFO(L"ONNX recognizer loaded — using InsightFace w600k_r50");
    } else {
        FACELOGIN_ERROR(L"ONNX recognizer failed to load — recognition unavailable");
        return false;
    }
    m_onnxRecognizer->SetLowLightEnhance(m_config.low_light_enhance);

    // Anti-spoof is a mandatory authentication component.  Never start the
    // service without it: falling back to identity-only matching lets a photo
    // release the stored Windows credential.
    m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
    std::wstring miniFasV2Path = m_modelsDir + L"\\MiniFASNetV2.onnx";
    std::wstring miniFasV1SePath = m_modelsDir + L"\\MiniFASNetV1SE.onnx";
    if (m_antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath)) {
        FACELOGIN_INFO(L"Anti-spoof models loaded (MiniFASNetV2 + MiniFASNetV1SE)");
    } else {
        // Keep the pipe online so LogonUI receives a precise AUTH_ERROR instead
        // of treating a missing service as a transient connectivity problem.
        // ProcessAuthRequest remains fail-closed until CONFIG_RELOAD restores
        // a valid model.
        FACELOGIN_ERROR(L"Anti-spoof model unavailable — authentication will remain disabled");
        m_antiSpoof.reset();
    }
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

    FACELOGIN_INFO(L"Liveness method: antispoof (mandatory, fail-closed; ready=%s)",
                   (m_antiSpoof && m_antiSpoof->IsInitialized()) ? L"yes" : L"no");
    FACELOGIN_INFO(L"Match threshold: %.3f", m_matchThreshold);
    FACELOGIN_INFO(L"Initialization complete");

    return true;
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
            m_store->LoadDatabase();
            m_pipeServer->WriteMessage(ipc::MSG_RELOAD_OK);
            m_pipeServer->Disconnect();
            FACELOGIN_INFO(L"Database reloaded");
        }
        else if (request == ipc::MSG_CONFIG_RELOAD) {
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
            if (m_livenessMethod == LivenessMethod::AntiSpoof && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
                m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
                std::wstring miniFasV2Path = m_modelsDir + L"\\MiniFASNetV2.onnx";
                std::wstring miniFasV1SePath = m_modelsDir + L"\\MiniFASNetV1SE.onnx";
                if (m_antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath)) {
                    FACELOGIN_INFO(L"CONFIG_RELOAD: dual MiniFAS PAD loaded successfully");
                } else {
                    FACELOGIN_ERROR(L"CONFIG_RELOAD: anti-spoof unavailable — authentication remains fail-closed");
                    m_antiSpoof.reset();
                }
            }
            // Low-light enhancement is recognition-only. PAD stays on its
            // calibrated raw-camera preprocessing path.
            m_onnxRecognizer->SetLowLightEnhance(m_config.low_light_enhance);
            const bool antiSpoofReady =
                m_antiSpoof && m_antiSpoof->IsInitialized();
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
                    if (!m_webcamDS->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
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
                    if (!m_webcamMF->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
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

    // Authentication must never degrade to identity-only matching.  This
    // check also covers a model that was removed/corrupted after startup and a
    // failed hot-reload attempt.
    if (m_livenessMethod != LivenessMethod::AntiSpoof ||
        !m_antiSpoof || !m_antiSpoof->IsInitialized()) {
        FACELOGIN_ERROR(L"Authentication refused: anti-spoof model is unavailable");
        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
            L"活体检测模块不可用，请使用密码登录并检查模型文件"));
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
    auto grabFrame = [this](dlib::matrix<dlib::rgb_pixel>& f) -> bool {
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

    // Drop initial frames to let camera exposure adjust. 5 frames is enough
    // (exposure settles within 3-5 frames); 10 frames wasted ~0.3s per auth.
    dlib::matrix<dlib::rgb_pixel> frame;
    for (int i = 0; i < 5; i++) {
        grabFrame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // STATUS: Notify credential provider that recognition has started.
    // L"\u8bc6\u522b\u4e2d..." = L"识别中..."
    {
        std::wstring statusMsg = std::wstring(ipc::MSG_STATUS_PREFIX) + L"\u8bc6\u522b\u4e2d...";
        m_pipeServer->WriteMessage(statusMsg);
    }

    auto startTime = std::chrono::steady_clock::now();
    bool authSent = false;
    int consecutiveMatches = 0;
    std::wstring consensusSid;
    static constexpr int CONSENSUS_FRAMES = 3;

    while (m_running) {
        // Abort early if the client (LogonUI) has gone away — e.g. the user
        // switched to password/fingerprint unlock. Otherwise we'd keep the
        // camera on until the timeout.
        if (m_pipeServer->IsClientDisconnected()) {
            FACELOGIN_INFO(L"Client disconnected during auth — aborting, releasing camera");
            return false;
        }

        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= m_authTimeoutSeconds) {
            FACELOGIN_INFO(L"Authentication timed out");
            m_pipeServer->WriteMessage(ipc::MSG_AUTH_TIMEOUT);
            FlushFileBuffers(m_pipeServer->GetHandle());
            m_pipeServer->DrainOutput(5000);
            return false;
        }

        if (!grabFrame(frame)) {
            if (!m_running) return false;
            // A stalled camera (e.g. after resume) self-shut-down in
            // GrabFrame. Rebuild it here so auth can continue instead of
            // spinning on a dead SourceReader until timeout.
            if (m_webcamMF && !m_webcamMF->IsInitialized()) {
                FACELOGIN_INFO(L"MF camera stalled — re-initializing");
                m_webcamMF->Shutdown();
                m_webcamMF.reset();
                m_webcamMF = std::make_unique<WebcamCapture>();
                if (!m_webcamMF->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
                    FACELOGIN_ERROR(L"MF camera re-init failed");
                    m_webcamMF.reset();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // (grabFrame above already applied camera rotation)

        // Face detection: SCRFD detects and yields the 5 keypoints directly
        // (gnkps variant) — no separate landmark model. Alignment to 112×112
        // happens inside ComputeEmbedding via a similarity transform.
        std::optional<CredentialStore::MatchResult> match;

        auto onnxDet = m_onnxDetector->DetectLargestFace(frame);
        if (!onnxDet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Embedding + match: ONNX (the only recognizer).
        auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(frame, onnxDet->kps);
        if (!onnxEmb.empty()) {
            // Pass the true dimensionality (512-D) so FindBestMatch compares
            // against same-dimension stored embeddings only.
            match = m_store->FindBestMatch(onnxEmb.data(), onnxEmb.size(), m_matchThreshold);
        }

        if (match) {
            if (match->sid.empty()) {
                FACELOGIN_ERROR(L"Matched credential has an empty SID — enrollment data is invalid");
                m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
                    L"身份数据无效，请使用密码登录并重新录入人脸"));
                FlushFileBuffers(m_pipeServer->GetHandle());
                m_pipeServer->DrainOutput(5000);
                SecureClearMatchPassword(match);
                return false;
            }

            // Consensus is meaningful only when every accepted frame belongs
            // to the same Windows account.  A different registered user starts
            // a fresh sequence instead of inheriting the previous count.
            if (consensusSid.empty() || consensusSid == match->sid) {
                consensusSid = match->sid;
                consecutiveMatches++;
            } else {
                FACELOGIN_WARN(L"Matched SID changed during consensus — resetting sequence");
                consensusSid = match->sid;
                consecutiveMatches = 1;
            }
            FACELOGIN_INFO(L"Face matched: %s (distance=%.4f) [%d/%d]",
                          match->username.c_str(), match->distance,
                          consecutiveMatches, CONSENSUS_FRAMES);

            if (consecutiveMatches < CONSENSUS_FRAMES) {
                SecureClearMatchPassword(match);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
        } else {
            // Soft consensus: a miss DECAYS the counter by 1 instead of fully
            // resetting to 0. A single intermittent bad frame (motion, blink,
            // momentary profile turn, partial occlusion) then no longer forces
            // a full 3-frame restart — the user's slightly moving face stays
            // matched and auth completes in ~1-2s instead of timing out.
            //
            // Security is preserved: this only relaxes the frame-consensus;
            // mandatory PAD AND the post-liveness final same-SID match verify
            // still run before credentials are released.
            if (consecutiveMatches > 0) {
                consecutiveMatches--;
                FACELOGIN_INFO(L"Match lost — counter decayed to %d", consecutiveMatches);
                if (consecutiveMatches == 0) consensusSid.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        {
            const std::wstring initialSid = consensusSid;

            if (initialSid.empty() || match->sid != initialSid) {
                FACELOGIN_ERROR(L"Authentication identity binding invariant failed");
                SecureClearMatchPassword(match);
                return false;
            }

            // Passwordless account: face login cannot unlock it (no password to
            // submit to LSA). Degrade gracefully with a notice instead of
            // attempting liveness and submitting nothing. Do NOT mark the user
            // as logged in.
            if (match->passwordless) {
                FACELOGIN_WARN(L"Matched passwordless account '%s' — face login cannot unlock; notifying CP",
                               match->username.c_str());
                m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(ipc::MSG_PASSWORDLESS_NOTICE));
                FlushFileBuffers(m_pipeServer->GetHandle());
                m_pipeServer->DrainOutput(5000);
                SecureClearMatchPassword(match);
                return false;
            }

            std::wstring domain = L".";
            wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD size = ARRAYSIZE(computerName);
            if (GetComputerNameW(computerName, &size)) {
                domain = computerName;
            }

            // === Liveness check ===
            {
                LivenessMethod method = m_livenessMethod;

                // Determine status text
                m_pipeServer->WriteMessage(std::wstring(ipc::MSG_STATUS_PREFIX) + L"\u6b63\u5728\u8fdb\u884c\u6d3b\u4f53\u68c0\u6d4b...");
                FlushFileBuffers(m_pipeServer->GetHandle());

                bool livenessPassed = false;
                bool livenessInferenceError = false;
                bool livenessIdentityMismatch = false;

                if (method == LivenessMethod::AntiSpoof) {
                    // Calibrated dual-model consensus: all five frames must pass.
                    int totalChecks = AntiSpoofCheckCount(m_antiSpoofThreshold);
                    int passRequired = AntiSpoofPassRequired(totalChecks);
                    FACELOGIN_INFO(L"Anti-spoof: threshold=%.3f → %d checks, %d required",
                                   m_antiSpoofThreshold, totalChecks, passRequired);
                    auto asStart = std::chrono::steady_clock::now();
                    int passCount = 0, totalChecked = 0;
                    while (m_running && totalChecked < totalChecks) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
                            SecureClearMatchPassword(match);
                            return false;
                        }
                        auto asElapsed = std::chrono::steady_clock::now() - asStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(asElapsed).count() >= 5) break;

                        dlib::matrix<dlib::rgb_pixel> asFrame;
                        if (!grabFrame(asFrame)) { if (!m_running) break; std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

                        // MiniFASNet consumes expanded crops around the SCRFD bbox.
                        auto asDet = m_onnxDetector->DetectLargestFace(asFrame);
                        if (!asDet) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

                        // Bind every PAD sample to the same Windows identity
                        // that won the initial consensus.  Otherwise user A's
                        // stored credential could be released after user B (or
                        // a swapped face) supplied the liveness frames.
                        auto continuityEmb =
                            m_onnxRecognizer->ComputeEmbedding(asFrame, asDet->kps);
                        if (continuityEmb.empty()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        auto continuityMatch = m_store->FindBestMatch(
                            continuityEmb.data(), continuityEmb.size(), m_matchThreshold);
                        if (!continuityMatch) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }
                        const bool sameIdentity = !continuityMatch->sid.empty() &&
                                                  continuityMatch->sid == initialSid;
                        SecureClearMatchPassword(continuityMatch);
                        if (!sameIdentity) {
                            FACELOGIN_WARN(L"Identity changed during anti-spoof — rejecting face swap");
                            livenessIdentityMismatch = true;
                            break;
                        }

                        float score = m_antiSpoof->Predict(asFrame,
                            dlib::rectangle(static_cast<long>(asDet->x1),
                                            static_cast<long>(asDet->y1),
                                            static_cast<long>(asDet->x2),
                                            static_cast<long>(asDet->y2)));
                        if (!std::isfinite(score) || score < 0.0f || score > 1.0f) {
                            FACELOGIN_ERROR(L"Anti-spoof inference returned invalid score: %.4f", score);
                            livenessInferenceError = true;
                            break;
                        }

                        totalChecked++;
                        if (score >= m_antiSpoofThreshold) passCount++; // config-driven threshold
                        FACELOGIN_INFO(L"Anti-spoof frame %d: score=%.3f (pass=%d)", totalChecked, score, passCount);

                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    // A partial sample set is not enough.  Previously one early
                    // passing frame could satisfy passRequired even when the
                    // remaining required frames were never captured.
                    livenessPassed = (!livenessInferenceError &&
                                      totalChecked == totalChecks &&
                                      passCount >= passRequired);
                    if (!livenessPassed) {
                        FACELOGIN_WARN(L"Anti-spoof check failed: %d/%d passed (need %d)",
                                       passCount, totalChecked, passRequired);
                    }
                }

                if (!livenessPassed) {
                    FACELOGIN_WARN(L"Liveness check failed");
                    m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
                        livenessInferenceError
                            ? L"活体检测模块异常，请使用密码登录"
                            : livenessIdentityMismatch
                            ? L"活体验证期间人脸不匹配，请重试"
                            : L"\u68c0\u6d4b\u5230\u653b\u51fb\uff0c\u8bf7\u4f7f\u7528\u771f\u5b9e\u4eba\u8138"));
                    FlushFileBuffers(m_pipeServer->GetHandle());
                    m_pipeServer->DrainOutput(5000);
                    SecureClearMatchPassword(match);
                    return false;
                }

                FACELOGIN_INFO(L"Liveness passed \u2014 verifying match");

                // Final match verify (prevents face-swap).
                //
                // Uses the SAME SCRFD detector as the recognition stage so the
                // two stages agree on face position. Retries over a short window:
                // the frame right after liveness is often mid-motion and its
                // box/embedding is noisy, so a single frame is unreliable. We
                // keep grabbing until a frame both detects a face AND matches
                // (or ~2s elapses).
                if (method == LivenessMethod::AntiSpoof) {
                    auto verifyStart = std::chrono::steady_clock::now();
                    bool verifyOk = false;
                    while (m_running && !verifyOk) {
                        if (m_pipeServer->IsClientDisconnected()) {
                            FACELOGIN_INFO(L"Client disconnected during final verify — aborting");
                            SecureClearMatchPassword(match);
                            return false;
                        }
                        auto vElapsed = std::chrono::steady_clock::now() - verifyStart;
                        if (std::chrono::duration_cast<std::chrono::seconds>(vElapsed).count() >= 2) break;

                        dlib::matrix<dlib::rgb_pixel> verifyFrame;
                        if (!grabFrame(verifyFrame)) {
                            if (!m_running) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        // Detect with SCRFD (primary), same as the recognition loop.
                        auto det = m_onnxDetector->DetectLargestFace(verifyFrame);
                        if (!det) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            continue;
                        }

                        std::optional<CredentialStore::MatchResult> verifyMatch;
                        auto verifyEmb = m_onnxRecognizer->ComputeEmbedding(verifyFrame, det->kps);
                        if (!verifyEmb.empty()) {
                            verifyMatch = m_store->FindBestMatch(verifyEmb.data(), verifyEmb.size(), m_matchThreshold);
                        }

                        if (verifyMatch) {
                            if (!verifyMatch->sid.empty() && verifyMatch->sid == initialSid) {
                                verifyOk = true;
                            } else {
                                FACELOGIN_WARN(L"Final verify matched a different SID — rejecting face swap");
                            }
                            // The final check only proves continuity.  Never
                            // replace the original credential with an arbitrary
                            // registered user's MatchResult.
                            SecureClearMatchPassword(verifyMatch);
                            if (verifyOk) break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }

                    if (!verifyOk) {
                        FACELOGIN_WARN(L"Final match verify failed \u2014 face swap detected");
                        m_pipeServer->WriteMessage(ipc::BuildAuthErrorMessage(
                            L"\u6d3b\u4f53\u9a8c\u8bc1\u671f\u95f4\u4eba\u8138\u4e0d\u5339\u914d\uff0c\u8bf7\u91cd\u8bd5"));
                        FlushFileBuffers(m_pipeServer->GetHandle());
                        m_pipeServer->DrainOutput(5000);
                        SecureClearMatchPassword(match);
                        return false;
                    }
                }
            }

            // Serialize credentials only after liveness and same-SID final
            // verification have both succeeded.
            std::wstring msg = ipc::BuildAuthSuccessMessage(
                match->sid, match->upn,
                domain, match->username, match->password);
            bool writeOk = m_pipeServer->WriteMessage(msg);
            FlushFileBuffers(m_pipeServer->GetHandle());
            SecureClearWideString(msg);
            SecureClearMatchPassword(match);

            if (!writeOk) {
                FACELOGIN_WARN(L"Failed to send authentication credentials");
                return false;
            }

            authSent = true;
            FACELOGIN_INFO(L"Credentials sent for %s\\%s",
                          domain.c_str(), match->username.c_str());

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
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    return authSent;
}

std::wstring FaceService::GetModelsDir() {
    {
        std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
        if (!regData.empty()) {
            return regData + L"\\models";
        }
    }
    wchar_t programData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        return std::wstring(programData) + L"\\FaceLogin\\models";
    }
    return L"C:\\ProgramData\\FaceLogin\\models";
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
