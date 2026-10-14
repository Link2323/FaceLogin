#include "EnrollmentWizard.h"
#include "enrollment_text.h"
#include "preview_exposure.h"
#include "preview_frame_encoder.h"
#include "enrollment_templates.h"
#include "enrollment_accounts.h"
#include "log_viewer.h"
#include "../common/logger.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/frame_image.h"
#include "../common/model_hashes.h"
#include "../common/sha256_util.h"
#include <shlobj.h>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "ole32.lib")

namespace facelogin {

using enrollment_detail::Utf8ToWstr;
using enrollment_detail::WstrToUtf8;

EnrollmentWizard::EnrollmentWizard() {
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
    CreateDirectoryW(m_dataDir.c_str(), nullptr);

    // The console's own log file, read back by the log viewer's "Console"
    // source like any other file-based source. v1.7.x named it enrollment.log:
    // MoveFile carries that file over on the first run of the renamed build
    // (fails silently when console.log already exists); pre-UTF-8 content is
    // still rotated aside by RotateAsideLegacyUtf16 inside SetLogFile.
    std::wstring logPath = m_dataDir + L"\\log\\console.log";
    MoveFileW((m_dataDir + L"\\log\\enrollment.log").c_str(), logPath.c_str());
    Logger::Instance().SetLogFile(logPath);
    Logger::Instance().SetMinLevel(LogLevel::Info);
    Logger::Instance().SetEnableDebugOutput(true);

    FACELOGIN_INFO(L"=== Enrollment Wizard started ===");

    m_frameEncoder = std::make_unique<PreviewFrameEncoder>();

    m_identity = session_identity::ReadSessionIdentity();

    m_webcam     = std::make_unique<WebcamCaptureDS>();
    m_accounts = std::make_unique<EnrollmentAccounts>(m_dataDir, m_identity, m_serviceClient);

    m_config = LoadConfig(m_dataDir);
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;
}

EnrollmentWizard::~EnrollmentWizard() {
    StopPreview();
}

// ============================================================================
// Preview Control
// ============================================================================

bool EnrollmentWizard::StartPreview() {
    if (m_previewRunning) return true;

    // The service/credential provider may still be releasing the camera just
    // after unlock. Absorb that short hand-off window instead of reporting a
    // spurious preview failure to the UI.
    constexpr int kInitRetries = 5;
    bool webcamOk = false;
    for (int attempt = 0; attempt < kInitRetries && !webcamOk; ++attempt) {
        webcamOk = m_webcam->Initialize(1280, 720,
                                        Utf8ToWstr(m_config.camera_device));
        if (!webcamOk && attempt + 1 < kInitRetries) {
            FACELOGIN_WARN(L"Webcam init attempt %d failed — retrying", attempt + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    if (!webcamOk) {
        FACELOGIN_ERROR(L"Failed to initialize webcam%s",
                        m_config.camera_device.empty() ? L"" : L" (configured device)");
        return false;
    }

    m_previewExposure = std::make_unique<PreviewExposure>(
        *m_webcam, m_dataDir + L"\\camera_tune.state");
    m_previewExposure->Begin();

    std::wstring modelsDir = m_dataDir + L"\\models";

    // Load SCRFD ONNX detector (gnkps variant — provides the 5 alignment
    // keypoints directly; no separate landmark model anymore).
    // 10g tier: ~3.4x faster than 34g at -1% WIDER Face (96.17→95.19).
    m_onnxDetector = std::make_unique<OnnxDetector>();
    std::wstring detPath = modelsDir + L"\\det_10g_gnkps.onnx";
    if (!m_onnxDetector->Initialize(detPath)) {
        FACELOGIN_ERROR(L"SCRFD detector failed to load — enrollment unavailable");
        m_webcam->Shutdown();
        return false;
    }

    // Load the pinned OpenCV SFace recognizer.
    m_onnxRecognizer = std::make_unique<OnnxRecognizer>();
    std::wstring onnxPath = modelsDir + L"\\face_recognition_sface_2021dec.onnx";
    if (!VerifyModelIntegrity(onnxPath, model_hashes::kRecognizer, L"OpenCV SFace recognizer") ||
        !m_onnxRecognizer->Initialize(onnxPath)) {
        FACELOGIN_ERROR(L"ONNX recognizer failed to load — enrollment unavailable");
        m_webcam->Shutdown();
        return false;
    }

    // Anti-spoof is mandatory for enrollment.  Allowing capture without it
    // would let a photograph become the trusted template for later logons.
    m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
    std::wstring miniFasV2Path = modelsDir + L"\\MiniFASNetV2.onnx";
    std::wstring miniFasV1SePath = modelsDir + L"\\MiniFASNetV1SE.onnx";
    if (!m_antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath)) {
        FACELOGIN_ERROR(L"Dual MiniFAS PAD unavailable — enrollment refused");
        m_antiSpoof.reset();
        m_webcam->Shutdown();
        return false;
    }
    if (!m_antiSpoof || !m_antiSpoof->IsInitialized()) {
        FACELOGIN_ERROR(L"No supported liveness method available — enrollment refused");
        m_webcam->Shutdown();
        return false;
    }

    m_previewExposure->Prepare(*m_onnxDetector, m_config.camera_rotation);

    FACELOGIN_INFO(L"Liveness method: antispoof (mandatory) | Preview started: 1280x720");

    m_previewRunning = true;
    m_frameRunning = true;

    // Single background thread: GrabFrame → JPEG encode → detect → update caches.
    // The UI thread stays completely free; JS polls the caches via GetLatest*().
    m_frameThread = std::thread([this]() {
        int reinitAttempts = 0;
        while (m_frameRunning) {
            FrameImage frame;
            unsigned long long frameSequence = 0;
            if (!m_webcam->GrabFrame(frame, &frameSequence)) {
                // GrabFrame self-shuts down after repeated failures, which is
                // common when a camera is taken over during lock/resume.
                // Rebuild it here with a bounded retry budget so the preview
                // recovers without spinning forever on a dead capture graph.
                if (!m_webcam->IsInitialized() && m_frameRunning) {
                    if (++reinitAttempts > 3) {
                        FACELOGIN_ERROR(L"Preview camera re-init exceeded limit");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (!m_frameRunning) break;
                    if (m_webcam->Initialize(1280, 720,
                                             Utf8ToWstr(m_config.camera_device))) {
                        FACELOGIN_INFO(L"Preview camera re-initialized (%d/3)",
                                       reinitAttempts);
                        m_previewExposure->CameraReopened();
                        reinitAttempts = 0;
                    } else {
                        FACELOGIN_WARN(L"Preview camera re-init %d/3 failed",
                                       reinitAttempts);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            reinitAttempts = 0;

            RotateFrame(frame, m_config.camera_rotation);

            std::string b64;
            m_frameEncoder->Encode(frame, b64);
            // Preview overlay: detect the face with SCRFD and show its box.
            std::string faceJson = "[]";
            if (m_onnxDetector) {
                auto det = m_onnxDetector->DetectLargestFace(frame);
                m_previewExposure->Observe(frame, det ? &*det : nullptr, frameSequence,
                    *m_onnxDetector, m_config.camera_rotation,
                    [this] { return !m_frameRunning || m_capture.IsRunning(); });
                if (det) {
                    std::vector<facelogin::FaceWithKps> faces;
                    FaceWithKps fwl;
                    fwl.rect = FaceRect(static_cast<long>(det->x1),
                                               static_cast<long>(det->y1),
                                               static_cast<long>(det->x2),
                                               static_cast<long>(det->y2));
                    for (int k = 0; k < 10; k++) fwl.kps[k] = det->kps[k];
                    faces.push_back(std::move(fwl));
                    faceJson = FacesToJson(faces);
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                m_latestFrameB64  = std::move(b64);
                m_latestFacesJson = std::move(faceJson);
                m_latestFrame     = frame;
                ++m_latestFrameSequence;
            }
        }
    });

    return true;
}

void EnrollmentWizard::StopPreview() {
    m_previewRunning = false;
    m_frameRunning = false;

    // Pause the DirectShow graph first, but keep COM references alive until
    // the worker threads have exited. Releasing the graph before join would
    // race with GrabFrame; waiting for join before pausing can
    // hang the UI when the camera was taken over by LogonUI.
    if (m_webcam)
        m_webcam->Pause();

    m_capture.Stop();
    if (m_frameThread.joinable())
        m_frameThread.join();

    if (m_webcam)
        m_webcam->Shutdown();
    m_previewExposure.reset();
}

// ============================================================================
// Per-frame Processing (called from timer callback)
// ============================================================================

std::string EnrollmentWizard::GetLatestFrameBase64() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    return m_latestFrameB64;
}

std::string EnrollmentWizard::GetLatestFacesJson() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    return m_latestFacesJson;
}

// Atomically return BOTH the current frame (JPEG base64) and its face overlay
// JSON. The frame thread updates them together under the same lock, so reading
// them in one call guarantees they belong to the SAME frame. The frontend uses
// this to draw the background and overlay from matching frames — otherwise the
// overlay could come from a newer frame than the displayed image, causing the
// face box/landmarks to drift from the visible face.
std::string EnrollmentWizard::GetLatestFrameAndFaces() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    std::string result = m_latestFrameB64;
    result += "\x1E";  // record separator
    result += m_latestFacesJson;
    return result;
}

// ============================================================================
// Face Detection -> JSON
// ============================================================================

std::string EnrollmentWizard::FacesToJson(
    const std::vector<facelogin::FaceWithKps>& faces) {
    std::ostringstream js;
    js << "[";
    for (size_t fi = 0; fi < faces.size(); fi++) {
        if (fi > 0) js << ",";
        const auto& f = faces[fi];
        js << "{"
           << "\"x\":" << f.rect.left()
           << ",\"y\":" << f.rect.top()
           << ",\"w\":" << static_cast<int>(f.rect.width())
           << ",\"h\":" << static_cast<int>(f.rect.height())
           << ",\"landmarks\":[";
        for (int i = 0; i < 5; i++) {
            if (i > 0) js << ",";
            js << static_cast<int>(f.kps[i * 2]) << ","
               << static_cast<int>(f.kps[i * 2 + 1]);
        }
        js << "]";
        // Estimated head yaw in degrees (positive = turned toward own left).
        // Exposed for the preview overlay and multi-angle enrollment gating.
        js << ",\"yaw\":" << EstimateYawDeg(f.kps);
        // While capturing, the JS overlay color-codes the yaw readout against
        // this angle's gate.
        if (m_capture.IsRunning())
            js << ",\"targetYaw\":" << m_capture.Status().targetYaw;
        js << "}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Enrollment actions (called from JS)
// ============================================================================

std::string EnrollmentWizard::GetUsername() const {
    // Return UPN for MSA accounts, SAM name for local accounts
    const std::wstring& display = m_identity.upn.empty() ? m_identity.username : m_identity.upn;
    int len = WideCharToMultiByte(CP_UTF8, 0, display.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, ' ');
    WideCharToMultiByte(CP_UTF8, 0, display.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

bool EnrollmentWizard::CaptureFaceSamples(int angleIndex) {
    if (!m_previewRunning || !m_antiSpoof || !m_onnxDetector || !m_onnxRecognizer) {
        FACELOGIN_ERROR(L"CaptureFaceSamples refused: preview or mandatory models are unavailable");
        return false;
    }
    return m_capture.Start(angleIndex, *m_onnxDetector, *m_onnxRecognizer,
        *m_antiSpoof, m_antiSpoofThreshold,
        [this](FrameImage& frame, std::uint64_t& sequence) {
            std::lock_guard<std::mutex> lock(m_frameCacheMutex);
            if (m_latestFrame.size() == 0 || m_latestFrameSequence == sequence) return false;
            frame = m_latestFrame;
            sequence = m_latestFrameSequence;
            return true;
        });
}

void EnrollmentWizard::CancelCapture() { m_capture.Cancel(); }

// ============================================================================
// Multi-angle capture status (polled by JS during capture)
// ============================================================================

std::string EnrollmentWizard::GetCaptureStatus() {
    const auto status = m_capture.Status();
    std::ostringstream js;
    js << "{\"angle\":" << status.angle
       << ",\"label\":\"" << WstrToUtf8(status.label) << "\""
       << ",\"targetYaw\":" << status.targetYaw
       << ",\"collected\":" << status.collected
       << ",\"target\":" << EnrollmentCapture::kAngleTargetFrames
       << ",\"yaw\":" << status.yaw
       << ",\"total\":" << status.total
       << ",\"livenessChecking\":" << (status.checking ? "true" : "false")
       << ",\"livenessPassed\":" << (status.passed ? "true" : "false")
       << ",\"cancelled\":" << (status.cancelled ? "true" : "false")
       << ",\"done\":" << (status.done ? "true" : "false") << "}";
    return js.str();
}

std::string EnrollmentWizard::GetUserSid() const {
    return WstrToUtf8(m_identity.sid);
}

bool EnrollmentWizard::ValidatePassword(const std::wstring& password) {
    return session_identity::ValidatePassword(m_identity, password);
}

bool EnrollmentWizard::SaveEnrollment(const std::wstring& password,
                                      const std::wstring& label) {
    return SaveEnrollmentImpl(password, label);
}

bool EnrollmentWizard::SaveEnrollmentImpl(const std::wstring& password,
                                          const std::wstring& label, bool append) {
    EnrollmentSamples samples;
    if (!m_capture.CopyCompletedSamples(samples)) return false;
    std::vector<EnrollmentTemplate> templates;
    if (!BuildEnrollmentTemplates(samples, templates)) return false;
    EnrollmentSaveOutcome outcome;
    const bool acknowledged = m_accounts->SaveTemplates(templates, password, label, append, outcome);
    if (outcome.persisted) {
        m_lastEnrollHint = std::move(outcome.hint);
        // A failed reload ACK does not undo the save or permit reusing its proof.
        m_capture.ClearSavedSamples();
    }
    return acknowledged;
}

int EnrollmentWizard::GetFaceCount() { return m_accounts->GetFaceCount(); }
std::string EnrollmentWizard::GetFacesJson() { return m_accounts->GetFacesJson(); }
bool EnrollmentWizard::SaveEnrollmentAppend(const std::wstring& label) {
    return SaveEnrollmentImpl(L"", label, true);
}
bool EnrollmentWizard::DeleteFace(int faceId) { return m_accounts->DeleteFace(faceId); }
bool EnrollmentWizard::ClearAllFaces() { return m_accounts->ClearAllFaces(); }
bool EnrollmentWizard::RenameFace(int faceId, const std::wstring& label) {
    return m_accounts->RenameFace(faceId, label);
}
int EnrollmentWizard::GetAccountTypeChanged() { return m_accounts->GetAccountTypeChanged(); }
std::string EnrollmentWizard::CheckAccountTypeChanged() { return m_accounts->CheckAccountTypeChanged(); }
bool EnrollmentWizard::RefreshAccountIdentity(const std::wstring& password) {
    return m_accounts->RefreshAccountIdentity(password);
}
bool EnrollmentWizard::ClearStaleAccountUpn() { return m_accounts->ClearStaleAccountUpn(); }
bool EnrollmentWizard::AutoRepairEmptyUpnOnStartup() { return m_accounts->AutoRepairEmptyUpnOnStartup(); }

// ============================================================================
// Camera device enumeration
// ============================================================================

std::string EnrollmentWizard::GetCameraList() {
    auto devices = WebcamCaptureDS::ListCameras();
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < devices.size(); i++) {
        if (i > 0) js << ",";
        // JSON-escape name/path (device paths contain backslashes).
        auto esc = [](const std::wstring& ws) -> std::string {
            std::string out;
            for (wchar_t ch : ws) {
                if (ch == L'"' || ch == L'\\') out.push_back('\\');
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) out.append(mb, static_cast<size_t>(n));
            }
            return out;
        };
        js << "{\"path\":\"" << esc(devices[i].devicePath)
           << "\",\"name\":\"" << esc(devices[i].friendlyName) << "\"}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Configuration
// ============================================================================

std::string EnrollmentWizard::GetConfig() const {
    return ConfigToJson(m_config);
}

std::string EnrollmentWizard::GetLearningStatus() {
    return m_serviceClient.QueryLearningStatus();
}

bool EnrollmentWizard::SetConfig(const std::string& json) {
    AppConfig newConfig = ConfigFromJson(json);
    // A running preview must already have the mandatory model.  Refuse to
    // mutate live settings if that invariant has somehow been broken.
    if (m_previewRunning && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_ERROR(L"SetConfig: anti-spoof unavailable while preview is running");
        return false;
    }

    bool cameraChanged = (newConfig.camera_device != m_config.camera_device);

    if (!SaveConfig(m_dataDir, newConfig)) {
        FACELOGIN_ERROR(L"Failed to save config.json");
        return false;
    }
    m_config = newConfig;
    m_antiSpoofThreshold = newConfig.anti_spoof_threshold;

    // Notify the service and require an explicit acknowledgement. Saving a
    // file is not enough: if the service cannot load the mandatory PAD model,
    // the UI must not claim that authentication settings are active.
    const bool reloadConfirmed = m_serviceClient.ReloadConfig();

    FACELOGIN_INFO(L"Configuration updated: thr=%.2f antiSpoof=%.3f rotation=%d",
                  m_config.match_threshold, m_config.anti_spoof_threshold,
                  m_config.camera_rotation);

    // If the camera selection changed and the preview is running, restart the
    // preview so the new camera takes effect immediately.
    if (cameraChanged && m_previewRunning) {
        FACELOGIN_INFO(L"Camera selection changed — restarting preview");
        RestartPreview();
    }

    return reloadConfirmed;
}

bool EnrollmentWizard::RestartPreview() {
    StopPreview();
    return StartPreview();
}

// ============================================================================
// Log Viewer
// ============================================================================

std::string EnrollmentWizard::GetLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::Console);
}

std::string EnrollmentWizard::GetServiceLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::Service);
}

std::string EnrollmentWizard::GetAuthWorkerLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::AuthWorker);
}

std::string EnrollmentWizard::GetCredentialProviderLogLines() {
    return ReadEnrollmentLog(m_dataDir, EnrollmentLogSource::CredentialProvider);
}

bool EnrollmentWizard::ClearFileLog(const std::wstring& source) {
    if (source == L"service") return ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::Service);
    if (source == L"worker") return ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::AuthWorker);
    if (source == L"cp") return ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::CredentialProvider);
    return false;
}

void EnrollmentWizard::ClearLog() {
    ClearEnrollmentLog(m_dataDir, EnrollmentLogSource::Console);
}

} // namespace facelogin
