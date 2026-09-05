#include "auth_worker.h"

#include "auth_pipeline.h"
#include "auth_worker_protocol.h"
#include "timer_resolution.h"
#include "webcam_capture_dshow.h"
#include "onnx_models.h"
#include "model_failure.h"
#include "performance_affinity.h"
#include "../common/data_path.h"
#include "../common/logger.h"
#include "../common/model_hashes.h"
#include "../common/sha256_util.h"

#include <psapi.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace facelogin {
namespace {

using auth_worker::Channel;
using auth_worker::Message;
using auth_worker::MessageType;
using auth_worker::ReadStatus;
using auth_worker::WorkerConfig;

struct WorkerModels {
    std::unique_ptr<OnnxDetector> detector;
    std::unique_ptr<OnnxRecognizer> recognizer;
    std::unique_ptr<OnnxAntiSpoof> antiSpoof;
};

bool IsLocalSystemProcess() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    bool isSystem = false;
    if (bytes != 0 && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        DWORD sidBytes = SECURITY_MAX_SID_SIZE;
        std::vector<BYTE> systemSid(sidBytes);
        if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid.data(), &sidBytes)) {
            isSystem = EqualSid(user->User.Sid, systemSid.data()) != FALSE;
        }
    }
    CloseHandle(token);
    return isSystem;
}

bool SendFatal(Channel& channel, const std::wstring& message) {
    FACELOGIN_ERROR(L"Authentication worker fatal: %s", message.c_str());
    return channel.Write(MessageType::Fatal, 0, auth_worker::EncodeWString(message));
}

std::unique_ptr<WorkerModels> LoadModels(const std::wstring& modelsDir,
                                         const WorkerConfig& config,
                                         ModelLoadFailure& failure) {
    auto models = std::make_unique<WorkerModels>();
    failure = ModelLoadFailure::Load;

    const std::wstring detectorPath = modelsDir + L"\\det_10g_gnkps.onnx";
    if (!VerifyModelIntegrity(detectorPath, model_hashes::kDetector, L"SCRFD detector")) {
        failure = ModelLoadFailure::DetectorIntegrity;
        return {};
    }
    models->detector = std::make_unique<OnnxDetector>();
    if (!models->detector->Initialize(detectorPath)) return {};

    const std::wstring recognizerPath = modelsDir + L"\\w600k_r50.onnx";
    if (!VerifyModelIntegrity(recognizerPath, model_hashes::kRecognizer,
                              L"InsightFace w600k_r50 recognizer")) {
        failure = ModelLoadFailure::RecognizerIntegrity;
        return {};
    }
    models->recognizer = std::make_unique<OnnxRecognizer>();
    if (!models->recognizer->Initialize(recognizerPath)) return {};
    models->recognizer->SetLowLightEnhance(config.lowLightEnhance);

    const std::wstring v2Path = modelsDir + L"\\MiniFASNetV2.onnx";
    const std::wstring v1SePath = modelsDir + L"\\MiniFASNetV1SE.onnx";
    if (!VerifyModelIntegrity(v2Path, model_hashes::kMiniFasV2, L"MiniFASNetV2 (PAD)") ||
        !VerifyModelIntegrity(v1SePath, model_hashes::kMiniFasV1Se, L"MiniFASNetV1SE (PAD)")) {
        failure = ModelLoadFailure::PadIntegrity;
        return {};
    }
    models->antiSpoof = std::make_unique<OnnxAntiSpoof>();
    if (!models->antiSpoof->Initialize(v2Path, v1SePath)) return {};
    failure = ModelLoadFailure::None;
    return models;
}

void LogWorkerResources(const wchar_t* stage) {
    DWORD handles = 0;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    constexpr double kMiB = 1024.0 * 1024.0;
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        FACELOGIN_INFO(L"Auth worker %s (handles=%lu, private=%.1f MiB, working_set=%.1f MiB)",
                       stage, handles,
                       static_cast<double>(counters.PrivateUsage) / kMiB,
                       static_cast<double>(counters.WorkingSetSize) / kMiB);
    } else {
        FACELOGIN_INFO(L"Auth worker %s (handles=%lu)", stage, handles);
    }
}

bool ReadExpected(Channel& channel, Message& message, DWORD timeoutMs,
                  MessageType expected, uint64_t requestId) {
    const ReadStatus status = channel.Read(message, timeoutMs);
    return status == ReadStatus::Ok && message.type == expected &&
           message.requestId == requestId;
}

int SendTerminalAndExit(Channel& channel, MessageType type, uint64_t requestId,
                        std::vector<uint8_t> payload, DWORD exitCode) {
    if (!channel.Write(type, requestId, payload)) return ERROR_BROKEN_PIPE;

    // This executable is a one-shot, Job-owned resource boundary.  WriteFile
    // has copied the validated terminal message into the anonymous-pipe buffer
    // before it returns.  Terminating this process now is intentional: normal
    // C++ destruction would synchronously call DirectShow Stop() and race the
    // parent servicing that reply, putting driver teardown back on the unlock
    // critical path.  Kernel process exit reclaims the graph, driver handles,
    // ONNX sessions and heaps; the parent Job remains the orphan guard.
    TerminateProcess(GetCurrentProcess(), exitCode);
    return static_cast<int>(exitCode);  // Only reachable if TerminateProcess fails.
}

} // namespace

int RunAuthenticationWorker(HANDLE parentToWorker, HANDLE workerToParent) {
    if (!IsLocalSystemProcess()) {
        return ERROR_ACCESS_DENIED;
    }

    Channel channel(parentToWorker, workerToParent);
    if (!channel.IsValid()) return ERROR_INVALID_HANDLE;
    if (!channel.Write(MessageType::Hello, 0)) return ERROR_BROKEN_PIPE;

    Message init;
    if (!ReadExpected(channel, init, 10000, MessageType::Init, 0)) {
        SendFatal(channel, L"工作进程初始化协议失败");
        return ERROR_INVALID_DATA;
    }

    WorkerConfig config;
    if (!auth_worker::DecodeConfig(init.payload, config)) {
        SendFatal(channel, L"工作进程配置无效");
        return ERROR_INVALID_DATA;
    }

    std::wstring dataDirReason;
    const std::wstring dataDir = ResolveSecureDataDir(L"", &dataDirReason);
    if (dataDir.empty()) {
        SendFatal(channel, L"模型数据目录验证失败");
        return ERROR_ACCESS_DENIED;
    }
    Logger::Instance().SetLogFile(dataDir + L"\\log\\auth_worker.log");
    Logger::Instance().SetMinLevel(LogLevel::Info);
    FACELOGIN_INFO(L"=== Authentication worker starting (pid=%lu, DirectShow) ===",
                   GetCurrentProcessId());

    ModelLoadFailure modelFailure = ModelLoadFailure::Load;
    auto models = LoadModels(dataDir + L"\\models", config, modelFailure);
    if (!models || !models->detector || !models->recognizer || !models->antiSpoof) {
        SendFatal(channel, ModelLoadFailureMessage(modelFailure));
        return ERROR_FILE_NOT_FOUND;
    }
    // Pay ORT's first-run cost (arena/thread-pool/per-shape planning) during
    // preload, not on the first authenticated frame.
    WarmupInference(*models->detector, *models->recognizer, *models->antiSpoof);

    // Camera preload during lock-screen idle: registry-level device
    // enumeration + graph skeleton (device-independent filters). The device
    // ACTIVATION (BindToObject) stays behind AUTH_START, so the camera is
    // not opened here. On preload failure the AUTH_START path falls back to
    // the full legacy construction.
    auto camera = std::make_unique<WebcamCaptureDS>();
    if (!camera->Preload(config.cameraDevice)) {
        FACELOGIN_WARN(L"Camera preload failed — full initialization at AUTH_START");
    }
    bool cameraReady = false;
    const auto initializeCamera = [&camera, &cameraReady, &config]() {
        if (cameraReady) return true;
        cameraReady = camera->Initialize(640, 480, config.cameraDevice);
        return cameraReady;
    };

    const auto releaseCamera = [&camera, &cameraReady]() {
        if (!cameraReady) return;
        camera->Pause();
        camera->Shutdown();
        cameraReady = false;
        LogWorkerResources(L"camera released");
    };

    // READY means only runtime/model preload. Camera device ACTIVATION is
    // hard-deferred until the one allowed AUTH_START (enumeration and the
    // graph skeleton are preloaded above without touching the device). The
    // ready-time resource snapshot is logged by the parent (ModelWorkerLoop)
    // from outside this process; the camera release/failure snapshots below
    // have no parent-side equivalent and stay here.
    if (!channel.Write(MessageType::Ready, 0)) return ERROR_BROKEN_PIPE;

    // The idle command loop picks AUTH_START up via PeekNamedPipe+Sleep(5)
    // polling; on the default ~15.6 ms tick that Sleep actually waits a full
    // tick (measured 2026-09-03: mean 6.6 ms dev / 9.1 ms slow machine, paid
    // before the camera even starts). Raise the timer resolution from here to
    // process exit: the one-shot worker terminates right after its terminal
    // message (TerminateProcess skips this destructor — the kernel reclaims
    // the resolution request at process exit), and on Win11 the raise only
    // affects this process, not the locked system's power draw.
    const ScopedTimerResolution fineTimer(1);

    // The worker is preloaded while the desktop is locked. It handles exactly
    // one AUTH_START, then exits so camera-driver and heap high-water are
    // reclaimed even if the user repeatedly retries face authentication.
    Message command;
    for (;;) {
        const ReadStatus status = channel.Read(command, 1000);
        if (status == ReadStatus::Timeout) continue;
        if (status != ReadStatus::Ok) return ERROR_BROKEN_PIPE;
        if (command.type == MessageType::Cancel && command.requestId == 0 &&
            command.payload.empty()) {
            releaseCamera();
            return ERROR_SUCCESS;
        }
        if (command.type != MessageType::StartAuth || command.requestId == 0 ||
            !command.payload.empty()) {
            SendFatal(channel, L"工作进程收到了无效认证命令");
            return ERROR_INVALID_DATA;
        }
        break;
    }

    // Preserve the established hybrid-CPU optimization for the complete
    // AUTH_START window, including camera activation and all inference. The
    // persistent parent is never affinity-pinned.
    const ScopedPerformanceCoreAffinity affinityGuard(GetPerformanceCoreMask());

    // Keep this one-shot worker out of Windows' power-throttling (EcoQoS)
    // policy for the auth burst — pinning the affinity does not. A/B measured
    // on the slow test laptop (2026-09-01, three instrumented builds): without
    // the opt-out ~80% of rounds ran the P-cores at 1466-1833 of 2200 MHz for
    // the whole loop (face detect 150-177 ms/frame, E2E ~1.9 s), on battery
    // AND occasionally on AC; with the opt-out every round held 2200 MHz
    // (detect 44-78 ms, E2E ~1.2 s). The heuristic targets this Session-0
    // background child exactly when the desktop is locked and idle. The
    // process self-terminates right after its terminal message, so the
    // unthrottled window is bounded to the single authentication; failure is
    // non-fatal — the burst merely runs at whatever clock the policy grants.
    PROCESS_POWER_THROTTLING_STATE unthrottle{};
    unthrottle.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    unthrottle.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    unthrottle.StateMask = 0;  // 0 = never throttle execution speed
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                               &unthrottle, sizeof(unthrottle))) {
        FACELOGIN_WARN(L"Power-throttle opt-out failed: %lu — authentication "
                       L"may run at reduced clocks", GetLastError());
    }

    const auto authStart = std::chrono::steady_clock::now();
    const auto cameraStart = authStart;
    if (!initializeCamera()) {
        channel.Write(MessageType::AuthFailed, command.requestId,
                      auth_worker::EncodeWString(L"摄像头不可用"));
        LogWorkerResources(L"camera initialization failed");
        return ERROR_DEVICE_NOT_AVAILABLE;
    }
    const auto cameraReadyAt = std::chrono::steady_clock::now();

    std::atomic<bool> channelHealthy{true};
    AuthPipelineCallbacks callbacks;
    callbacks.grabFrame = [&camera](FrameImage& frame, unsigned long long& frameSequence) {
        frameSequence = 0;
        return camera->GrabFrame(frame, &frameSequence);
    };
    callbacks.isCancelled = [&channelHealthy]() { return !channelHealthy.load(); };
    callbacks.isClientDisconnected = []() { return false; };
    callbacks.reportStatus = [&channel, &channelHealthy, requestId = command.requestId](const std::wstring& text) {
        if (!channel.Write(MessageType::Status, requestId, auth_worker::EncodeWString(text))) {
            channelHealthy.store(false);
        }
    };
    callbacks.verifyBinding = [&channel, &channelHealthy, requestId = command.requestId](
        const std::vector<float>& embedding, unsigned int bindingIndex, float preNorm,
        float yawDeg, float pitchDeg) {
        if (!channelHealthy.load() ||
            !channel.Write(MessageType::MatchProbe, requestId,
                           auth_worker::EncodeMatchProbe(bindingIndex, embedding,
                                                         preNorm, yawDeg, pitchDeg))) {
            channelHealthy.store(false);
            return BindingDecision{BindingDecisionKind::Reject,
                                   L"认证通信失败，请使用密码登录"};
        }

        Message response;
        const ReadStatus read = channel.Read(response, 2000, 1);
        if (read != ReadStatus::Ok || response.requestId != requestId) {
            channelHealthy.store(false);
            return BindingDecision{BindingDecisionKind::Reject,
                                   L"认证服务未响应，请使用密码登录"};
        }
        if (response.type == MessageType::MatchAccept && response.payload.empty()) {
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }
        if (response.type == MessageType::MatchRetry && response.payload.empty()) {
            return BindingDecision{BindingDecisionKind::Retry, {}};
        }
        if (response.type == MessageType::MatchReject) {
            std::wstring error;
            if (auth_worker::DecodeWString(response.payload, error)) {
                return BindingDecision{BindingDecisionKind::Reject, std::move(error)};
            }
        }
        channelHealthy.store(false);
        return BindingDecision{BindingDecisionKind::Reject,
                               L"认证通信协议异常，请使用密码登录"};
    };

    AuthPipeline pipeline(*models->detector, *models->recognizer, *models->antiSpoof,
                          AuthPipelineConfig{config.antiSpoofThreshold,
                                             config.authTimeoutSeconds,
                                             config.cameraRotation},
                          std::move(callbacks));
    const AuthPipelineResult result = pipeline.Run();
    const auto pipelineDoneAt = std::chrono::steady_clock::now();
    const auth_worker::AuthTiming timing{
        static_cast<float>(std::chrono::duration<double, std::milli>(
            cameraReadyAt - cameraStart).count()),
        static_cast<float>(std::chrono::duration<double, std::milli>(
            pipelineDoneAt - cameraReadyAt).count()),
        static_cast<float>(std::chrono::duration<double, std::milli>(
            pipelineDoneAt - authStart).count())};

    if (!channelHealthy.load() || result.cancelled) {
        releaseCamera();
        return ERROR_CANCELLED;
    }

    // Do not synchronously stop a DirectShow graph before the terminal reply:
    // some drivers spend hundreds of milliseconds in IMediaControl::Stop(),
    // which would put camera teardown on the unlock critical path.
    if (result.succeeded) {
        return SendTerminalAndExit(channel, MessageType::AuthSucceeded,
                                   command.requestId,
                                   auth_worker::EncodeAuthTiming(timing),
                                   ERROR_SUCCESS);
    }
    FACELOGIN_INFO(L"Auth worker timing: camera_init=%.1f ms, pipeline=%.1f ms, total=%.1f ms",
                   timing.cameraInitMs, timing.pipelineMs, timing.totalMs);
    if (result.timedOut) {
        FACELOGIN_INFO(L"Auth worker sending timeout before process reclamation");
        return SendTerminalAndExit(channel, MessageType::AuthTimedOut,
                                   command.requestId, {}, ERROR_TIMEOUT);
    }
    FACELOGIN_INFO(L"Auth worker sending failure before process reclamation");
    return SendTerminalAndExit(channel, MessageType::AuthFailed, command.requestId,
                               auth_worker::EncodeWString(result.errorMessage),
                               ERROR_ACCESS_DENIED);
}

} // namespace facelogin
