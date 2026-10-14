#include "EnrollmentWizard.h"
#include "enrollment_text.h"
#include "log_viewer.h"
#include "../common/logger.h"
#include "../common/dpapi_util.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/frame_image.h"
#include "../common/model_hashes.h"
#include "../common/sha256_util.h"
#include "../vision/face_gain_tune.h"
#include <comdef.h>
#include <shlobj.h>
#include <wincodec.h>
#include <thread>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

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

    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&m_wicFactory));

    m_identity = session_identity::ReadSessionIdentity();

    m_webcam     = std::make_unique<WebcamCaptureDS>();
    m_store.SetDataDir(m_dataDir);

    m_config = LoadConfig(m_dataDir);
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;
}

EnrollmentWizard::~EnrollmentWizard() {
    StopPreview();
    m_capturing = false;
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_wicFactory) m_wicFactory->Release();
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

    // Replay the persisted last-good combo before the models load: the
    // driver intermittently forgets manual UVC controls across idle
    // power-downs (2026-09-13), and the model-load window absorbs the
    // exposure write's settle lag, so the tune check below measures the
    // replayed state instead of the transition.
    const std::wstring tuneStatePath = m_dataDir + L"\\camera_tune.state";
    const auto replayAt = std::chrono::steady_clock::now();
    SensorKnobs previewKnobs = BuildSensorKnobs();
    bool replayWrote = false;
    const bool usingAutoExposure =
        PreferAutoExposureWithoutGain(previewKnobs, &replayWrote);
    {
        TuneKnobState saved;
        if (!usingAutoExposure && LoadTuneKnobState(tuneStatePath, saved)) {
            replayWrote = ApplyTuneKnobState(previewKnobs, saved);
        }
    }

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

    m_exposureRecovery = {};
    // Face-priority gain tune — same policy as the auth worker
    // (face_gain_tune.h): under the driver's full-frame-average AE a small
    // backlit face stays dark, and the preview must show (and enrollment
    // capture) the same brightened domain unlock will see. No face in frame
    // skips tuning unless the scene is clipped or centrally dark; the watch retries while preview
    // runs, and the next StartPreview re-tunes otherwise.
    {
        const auto noCancel = [] { return false; };
        GainTuneConfig tuneCfg;
        tuneCfg.persistPath = tuneStatePath;
        tuneCfg.recovery = &m_exposureRecovery;
        // Fast model loads must not let a shortening/gain write race replay.
        // No pre-replay luma was measured, so early response cannot be proven.
        while (replayWrote && std::chrono::steady_clock::now() - replayAt <
                                 std::chrono::milliseconds(tuneCfg.settleMs)) {
            FrameImage discard;
            m_webcam->GrabFrame(discard);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        TuneFaceExposure(BuildSensorKnobs(),
                         [this](FrameImage& f, unsigned long long& s) {
                             return m_webcam->GrabFrame(f, &s);
                         },
                         *m_onnxDetector, noCancel, m_config.camera_rotation,
                         tuneCfg);
    }
    // The tune above just moved the sensor: hold the streaming watch off for
    // its cooldown so it doesn't re-judge the settle frames.
    m_lastExposureTune = std::chrono::steady_clock::now();
    m_lastExposureWatch = m_lastExposureTune;
    m_pendingWatchLuma = -1.0f;

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
                        m_exposureRecovery = {};
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

            std::string b64 = EncodeJPEGBase64(frame);
            // Preview overlay: detect the face with SCRFD and show its box.
            std::string faceJson = "[]";
            if (m_onnxDetector) {
                auto det = m_onnxDetector->DetectLargestFace(frame);
                WatchPreviewExposure(frame, det ? &*det : nullptr, frameSequence);
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

// ============================================================================
// Streaming exposure governor (frame thread)
// ============================================================================

namespace {

constexpr std::chrono::seconds kExposureWatchInterval(3);
constexpr std::chrono::seconds kExposureTuneCooldown(10);

} // namespace

// Driver-facing exposure/gain controls shared by the StartPreview tune and the
// frame-thread streaming watch.
SensorKnobs EnrollmentWizard::BuildSensorKnobs() {
    SensorKnobs knobs;
    knobs.exposureRange = [this](long& mn, long& mx, long& st) {
        return m_webcam->GetExposureRange(&mn, &mx, &st);
    };
    knobs.exposureGet = [this](long& v) { return m_webcam->GetExposure(&v); };
    knobs.exposureSet = [this](long v) { return m_webcam->SetExposure(v); };
    knobs.exposureSetAuto = [this](long v) { return m_webcam->SetExposure(v, false); };
    knobs.exposureIsManual = [this](bool& manual) {
        long value = 0;
        return m_webcam->GetExposure(&value, &manual);
    };
    knobs.gainRange = [this](long& mn, long& mx, long& st) {
        return m_webcam->GetGainRange(&mn, &mx, &st);
    };
    knobs.gainGet = [this](long& v) { return m_webcam->GetGain(&v); };
    knobs.gainSet = [this](long v) { return m_webcam->SetGain(v); };
    return knobs;
}

void EnrollmentWizard::WatchPreviewExposure(const FrameImage& frame,
                                            const OnnxDetector::Detection* det,
                                            unsigned long long frameSequence) {
    // A capture in flight must see one stable capture domain — never move the
    // sensor between its PAD/embedding frames.
    if (m_capturing) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastExposureWatch < kExposureWatchInterval) return;
    m_lastExposureWatch = now;
    GainTuneConfig cfg;
    cfg.persistPath = m_dataDir + L"\\camera_tune.state";
    cfg.recovery = &m_exposureRecovery;
    if (det && m_exposureRecovery.pending) {
        const float luma = FaceBoxLuma(frame, *det);
        if (luma >= cfg.minFaceLuma && luma <= cfg.maxFaceLuma) {
            // Retry on the watch cadence, independently of identity/PAD and
            // the write cooldown. A failed check never refreshes the cache.
            m_exposureRecovery.attempted = false;
            ConfirmPendingFaceTune(BuildSensorKnobs(),
                [this](FrameImage& f, unsigned long long& s) { return m_webcam->GrabFrame(f, &s); },
                *m_onnxDetector, [this] { return !m_frameRunning || m_capturing; },
                m_config.camera_rotation, frameSequence, cfg);
            return;
        }
    }
    if (now - m_lastExposureTune < kExposureTuneCooldown) return;
    if (PreferAutoExposureWithoutGain(BuildSensorKnobs())) {
        m_pendingWatchLuma = -1.0f;
        return;
    }
    if (!det) {
        m_pendingWatchLuma = -1.0f;
        if (!IsSceneWhiteout(frame) && !IsSceneDark(frame, cfg)) return;
        m_lastExposureTune = now;
        TuneFaceExposure(BuildSensorKnobs(),
                         [this](FrameImage& f, unsigned long long& s) {
                             return m_webcam->GrabFrame(f, &s);
                         }, *m_onnxDetector,
                         [this] { return !m_frameRunning || m_capturing; },
                         m_config.camera_rotation, cfg);
        return;
    }
    const float luma = FaceBoxLuma(frame, *det);
    const bool outOfBand =
        luma >= 0.0f && (luma < cfg.minFaceLuma || luma > cfg.maxFaceLuma ||
                        detail::CanShortenExposure(luma, BuildSensorKnobs(), cfg));
    if (!outOfBand) {
        m_pendingWatchLuma = -1.0f;
        return;
    }    // Two consecutive out-of-band cycles (3 s apart): single readings jitter
    // with face fill ratio, and one spurious hit must not move the sensor.
    if (m_pendingWatchLuma < 0.0f ||
        std::fabs(luma - m_pendingWatchLuma) > 40.0f) {
        m_pendingWatchLuma = luma;
        return;
    }
    m_pendingWatchLuma = -1.0f;
    m_lastExposureTune = now;
    FACELOGIN_INFO(L"Preview exposure watch: face luma %.0f (band %.0f–%.0f), correction/shortening "
                   L"(twice) — re-tuning",
                   luma, cfg.minFaceLuma, cfg.maxFaceLuma);
    // Runs inline on the frame thread: the preview image pauses for the tune
    // budget (hundreds of ms) once per drift event — invisible next to a
    // washed-out preview. The trigger luma is this frame's; the tune grabs
    // its own fresh settle frames.
    TuneFaceExposure(luma, BuildSensorKnobs(),
                     [this](FrameImage& f, unsigned long long& s) {
                         return m_webcam->GrabFrame(f, &s);
                     },
                     *m_onnxDetector, [] { return false; },
                     m_config.camera_rotation, cfg);
}

void EnrollmentWizard::StopPreview() {
    m_previewRunning = false;
    m_frameRunning = false;
    m_capturing = false;

    // Pause the DirectShow graph first, but keep COM references alive until
    // the worker threads have exited. Releasing the graph before join would
    // race with GrabFrame; waiting for join before pausing can
    // hang the UI when the camera was taken over by LogonUI.
    if (m_webcam)
        m_webcam->Pause();

    if (m_captureThread.joinable())
        m_captureThread.join();
    if (m_frameThread.joinable())
        m_frameThread.join();

    if (m_webcam)
        m_webcam->Shutdown();
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
// JPEG Encoding via WIC
// ============================================================================

static std::string EncodeBase64(const BYTE* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? tbl[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? tbl[n & 63] : '=');
    }
    return out;
}

std::string EnrollmentWizard::EncodeJPEGBase64(const FrameImage& frame) {
    if (!m_wicFactory) return {};

    int srcW = static_cast<int>(frame.nc());
    int srcH = static_cast<int>(frame.nr());

    std::vector<BYTE> bgra(srcW * srcH * 4);
    for (int y = 0; y < srcH; y++) {
        BYTE* row = bgra.data() + y * srcW * 4;
        for (int x = 0; x < srcW; x++) {
            const auto& p = frame(y, x);
            row[x * 4 + 0] = p.blue;
            row[x * 4 + 1] = p.green;
            row[x * 4 + 2] = p.red;
            row[x * 4 + 3] = 255;
        }
    }

    IWICBitmap* pBitmap = nullptr;
    HRESULT hr = m_wicFactory->CreateBitmapFromMemory(
        srcW, srcH, GUID_WICPixelFormat32bppBGR,
        srcW * 4, static_cast<UINT>(bgra.size()), bgra.data(), &pBitmap);
    if (FAILED(hr)) return {};

    IWICBitmapEncoder* pEncoder = nullptr;
    hr = m_wicFactory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &pEncoder);
    if (FAILED(hr)) { pBitmap->Release(); return {}; }

    IStream* pStream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
    hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { pEncoder->Release(); pStream->Release(); pBitmap->Release(); return {}; }

    IWICBitmapFrameEncode* pFrameEncode = nullptr;
    IPropertyBag2* pProps = nullptr;
    hr = pEncoder->CreateNewFrame(&pFrameEncode, &pProps);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.70f;
        pProps->Write(1, &opt, &v);
        VariantClear(&v);
        pFrameEncode->Initialize(pProps);
        pFrameEncode->SetSize(srcW, srcH);
        pFrameEncode->WriteSource(pBitmap, nullptr);
        pFrameEncode->Commit();
        pEncoder->Commit();
    }

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    ULONG jpgSize = static_cast<ULONG>(stat.cbSize.QuadPart);
    std::vector<BYTE> jpgData(jpgSize);
    LARGE_INTEGER li = {};
    pStream->Seek(li, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    pStream->Read(jpgData.data(), jpgSize, &read);

    std::string result = "data:image/jpeg;base64,";
    result += EncodeBase64(jpgData.data(), jpgSize);

    if (pFrameEncode) pFrameEncode->Release();
    if (pProps) pProps->Release();
    pEncoder->Release();
    pStream->Release();
    pBitmap->Release();

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
        if (m_capturing)
            js << ",\"targetYaw\":" << kAngleTargets[m_captureAngle];
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
    if (angleIndex < 0 || angleIndex > 2) {
        FACELOGIN_ERROR(L"CaptureFaceSamples: invalid angle %d", angleIndex);
        return false;
    }

    // COM methods can be invoked independently of the intended HTML flow.
    // Enforce the full capture boundary here so a caller cannot start a side
    // capture after StartPreview failed and thereby bypass anti-spoofing.
    if (!m_previewRunning || !m_antiSpoof || !m_antiSpoof->IsInitialized() ||
        !m_onnxDetector || !m_onnxDetector->IsInitialized() ||
        !m_onnxRecognizer || !m_onnxRecognizer->IsInitialized()) {
        FACELOGIN_ERROR(L"CaptureFaceSamples refused: preview or mandatory models are unavailable");
        return false;
    }

    // Join the previous capture thread if one exists — REQUIRED before
    // reassigning m_captureThread (an un-joined joinable thread terminates
    // the process). If it is still running (m_capturing stuck true, e.g. the
    // user clicked Start again mid-capture or a prior capture was interrupted
    // by a page switch), wait for it instead of silently returning false —
    // the capture loop has its own fail counters, so this join is bounded
    // (~20s worst case).
    if (m_captureThread.joinable()) {
        if (m_capturing)
            FACELOGIN_WARN(L"CaptureFaceSamples: previous capture still running — waiting for it to finish");
        m_captureThread.join();
        m_capturing = false;
    }

    m_capturing = true;
    m_captureCancelled = false;
    m_captureAngle = angleIndex;
    if (angleIndex == 0) {
        // New capture round: reset everything. Later angles append to the
        // flat m_embeddings vector (grouped in angle order).
        m_samplesCollected = 0;
        m_embeddings.clear();
        m_angleSampleCounts[0] = m_angleSampleCounts[1] = m_angleSampleCounts[2] = 0;
    } else {
        // Non-front angle: keep the embeddings captured for EARLIER angles
        // (multi-angle continuation) but drop stale samples left in this
        // angle's slot by a previous round. Without this trim, a single-angle
        // re-capture of the same angle after a save/cancel would see the
        // leftover count == target and the loop below would finish instantly
        // with zero new frames, jumping straight to the confirm screen.
        size_t prior = 0;
        for (int a = 0; a < angleIndex; a++)
            prior += static_cast<size_t>(m_angleSampleCounts[a]);
        if (m_embeddings.size() > prior)
            m_embeddings.resize(prior);
        m_angleSampleCounts[angleIndex] = 0;
    }
    m_livenessPassed = false;
    m_livenessChecking = true;

    m_captureThread = std::thread([this, angleIndex]() {
        // Phase 1: every independently callable capture must prove liveness.
        // In particular, side-angle append is exposed through COM and cannot
        // inherit a stale proof from an earlier front capture or save.
        bool livenessPassed = false;
        bool livenessInferenceError = false;
        std::uint64_t lastFrameSequence = 0;

        // The preview and capture threads run independently. Consume each
        // cached camera frame at most once so a stalled preview cannot turn one
        // PAD result into a synthetic 5/5 sequence.
        const auto copyFreshFrame = [this, &lastFrameSequence](
            FrameImage& output) -> bool {
            std::lock_guard<std::mutex> lock(m_frameCacheMutex);
            if (m_latestFrame.size() == 0 ||
                m_latestFrameSequence == lastFrameSequence) {
                return false;
            }
            output = m_latestFrame;
            lastFrameSequence = m_latestFrameSequence;
            return true;
        };

        FACELOGIN_INFO(L"Enrollment: starting mandatory anti-spoof check for angle %d",
                       angleIndex);

        if (m_antiSpoof && m_antiSpoof->IsInitialized()) {
            // Enrollment keeps the calibrated five-frame gate regardless of
            // the fast_unlock preference: that switch trades liveness
            // evidence for speed at unlock time only.
            int totalChecks = AntiSpoofCheckCount(/*fastUnlock=*/false);
            int passRequired = AntiSpoofPassRequired(totalChecks);
            FACELOGIN_INFO(L"Enrollment anti-spoof: threshold=%.3f → %d checks, %d required",
                           m_antiSpoofThreshold, totalChecks, passRequired);
            auto asStart = std::chrono::steady_clock::now();
            int passCount = 0, totalChecked = 0;
            while (m_capturing && totalChecked < totalChecks) {
                auto elapsed = std::chrono::steady_clock::now() - asStart;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 8) break;

                FrameImage frame;
                if (!copyFreshFrame(frame)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    continue;
                }
                // MiniFASNet consumes expanded crops around the SCRFD bbox.
                auto asDet = m_onnxDetector->DetectLargestFace(frame);
                if (!asDet) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }

                float score = m_antiSpoof->Predict(frame,
                    FaceRect(static_cast<long>(asDet->x1),
                                    static_cast<long>(asDet->y1),
                                    static_cast<long>(asDet->x2),
                                    static_cast<long>(asDet->y2)));
                if (!std::isfinite(score) || score < 0.0f || score > 1.0f) {
                    FACELOGIN_ERROR(L"Enrollment anti-spoof returned invalid score: %.4f", score);
                    livenessInferenceError = true;
                    break;
                }

                totalChecked++;
                if (score >= m_antiSpoofThreshold) passCount++; // config-driven threshold
                FACELOGIN_INFO(L"Enrollment anti-spoof frame %d: score=%.3f (pass=%d)",
                              totalChecked, score, passCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            livenessPassed = (!livenessInferenceError &&
                              totalChecked == totalChecks &&
                              passCount >= passRequired);
        } else {
            FACELOGIN_ERROR(L"Enrollment refused: anti-spoof is not initialized");
        }

        m_livenessChecking = false;

        if (!livenessPassed) {
            if (m_captureCancelled) {
                FACELOGIN_INFO(L"Enrollment capture cancelled during liveness for angle %d",
                               angleIndex);
            } else {
                FACELOGIN_WARN(L"Enrollment liveness check failed");
            }
            m_capturing = false;
            return;
        }

        // Phase 2: collect yaw-gated face samples for this angle. Every frame
        // that contributes to the stored embedding is independently checked
        // by PAD on that exact image. This closes the live-then-photo swap
        // window that exists when liveness and enrollment use disjoint frames.
        const int targetYaw = kAngleTargets[angleIndex];
        constexpr float kYawTolerance = 10.0f;
        constexpr float kMinFaceSizePx = 60.0f;
        constexpr float kMinDetScore = 0.5f;

        int failCount = 0;
        while (m_angleSampleCounts[angleIndex] < kAngleTargetFrames && m_capturing) {
            // Read the latest frame from the frame-grab thread (no camera contention)
            FrameImage frame;
            if (!copyFreshFrame(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                continue;
            }

            // Detect with SCRFD — the 5 keypoints drive the alignment inside
            // ComputeEmbedding (no landmark model).
            auto onnxDet = m_onnxDetector->DetectLargestFace(frame);
            if (!onnxDet) {
                if (++failCount > 600) { m_capturing = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // Yaw gate: only frames near the target angle count as samples.
            m_lastYaw = EstimateYawDeg(onnxDet->kps);
            const float faceW = onnxDet->x2 - onnxDet->x1;
            if (std::abs(m_lastYaw - targetYaw) > kYawTolerance ||
                faceW < kMinFaceSizePx || onnxDet->score < kMinDetScore) {
                failCount = 0;   // waiting for the user to turn — not a failure
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            const float samplePadScore = m_antiSpoof->Predict(frame,
                FaceRect(static_cast<long>(onnxDet->x1),
                                static_cast<long>(onnxDet->y1),
                                static_cast<long>(onnxDet->x2),
                                static_cast<long>(onnxDet->y2)));
            if (!std::isfinite(samplePadScore) || samplePadScore < 0.0f ||
                samplePadScore > 1.0f || samplePadScore < m_antiSpoofThreshold) {
                FACELOGIN_WARN(L"Enrollment sample PAD failed closed for angle %d: score=%.4f",
                               angleIndex, samplePadScore);
                livenessPassed = false;
                break;
            }

            // Compute the embedding with the pinned SFace recognizer.
            // Store the FULL 128-D SFace embedding (no truncation). emb_norm is the
            // pre-normalization L2 norm (quality signal, progressive-learning
            // phase 0 calibration).
            float embNorm = 0.0f;
            auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(frame, onnxDet->kps,
                                                              &embNorm);
            if (onnxEmb.empty()) {
                if (++failCount > 600) { m_capturing = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            std::vector<float> emb = onnxEmb;
            FACELOGIN_INFO(L"Enrollment sample: angle=%d yaw=%.0f pad=%.3f emb_norm=%.3f",
                           angleIndex, m_lastYaw.load(), samplePadScore, embNorm);

            failCount = 0;
            m_embeddings.push_back(std::move(emb));
            m_angleSampleCounts[angleIndex]++;
            m_samplesCollected++;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        const bool complete = livenessPassed &&
            m_angleSampleCounts[angleIndex] == kAngleTargetFrames;
        m_livenessPassed = complete;
        if (m_captureCancelled) {
            FACELOGIN_INFO(L"Enrollment capture cancelled for angle %d", angleIndex);
        } else if (!complete) {
            FACELOGIN_WARN(L"Enrollment capture incomplete for angle %d: %d/%d samples",
                           angleIndex, m_angleSampleCounts[angleIndex].load(),
                           kAngleTargetFrames);
        }
        // Release-publishes the completed embedding vector to SaveEnrollment.
        m_capturing.store(false, std::memory_order_release);
    });

    return true;
}

// Cooperative stop for the JS cancel button: both capture phases check
// m_capturing between frames, so the thread exits within one poll interval
// (~33-150ms). The cancelled flag lets GetCaptureStatus distinguish a user
// cancel from a liveness failure until the next CaptureFaceSamples resets it.
void EnrollmentWizard::CancelCapture() {
    if (m_capturing) {
        FACELOGIN_INFO(L"Enrollment: capture cancelled by user");
        m_captureCancelled = true;
        m_capturing = false;
    }
}

// Helper: UTF-8 string with JSON escaping — safe to embed directly inside a
// JSON string literal for the face list and account-change responses. Escapes
// per RFC 8259: quote, backslash, and the mandatory control characters
// (U+0000–U+001F). Forward slash is intentionally NOT escaped (legal in JSON
// and never needed for these fields). Other characters pass through UTF-8.
static std::string WstrToUtf8Escaped(const std::wstring& ws) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (wchar_t ch : ws) {
        if (ch == L'"')            out += "\\\"";
        else if (ch == L'\\')      out += "\\\\";
        else if (ch == L'\b')      out += "\\b";
        else if (ch == L'\f')      out += "\\f";
        else if (ch == L'\n')      out += "\\n";
        else if (ch == L'\r')      out += "\\r";
        else if (ch == L'\t')      out += "\\t";
        else if (ch < 0x20) {      // other C0 control chars → \uXXXX
            out += "\\u00";
            out += kHex[(ch >> 4) & 0xF];
            out += kHex[ch & 0xF];
        }
        else {
            char mb[4] = {};
            int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
            if (n > 0) out.append(mb, static_cast<size_t>(n));
        }
    }
    return out;
}

// ============================================================================
// Multi-angle capture status (polled by JS during capture)
// ============================================================================

std::string EnrollmentWizard::GetCaptureStatus() {
    static constexpr const wchar_t* kAngleLabels[3] = {L"正面", L"左转", L"右转"};
    std::ostringstream js;
    const int angle = m_captureAngle.load();
    const int counts[3] = {
        m_angleSampleCounts[0].load(),
        m_angleSampleCounts[1].load(),
        m_angleSampleCounts[2].load()
    };
    const int total = counts[0] + counts[1] + counts[2];
    js << "{\"angle\":" << angle
       << ",\"label\":\"" << WstrToUtf8(kAngleLabels[angle]) << "\""
       << ",\"targetYaw\":" << kAngleTargets[angle]
       << ",\"collected\":" << counts[angle]
       << ",\"target\":" << kAngleTargetFrames
       << ",\"yaw\":" << m_lastYaw
       << ",\"total\":" << total
       << ",\"livenessChecking\":" << (m_livenessChecking ? "true" : "false")
       << ",\"livenessPassed\":" << (m_livenessPassed ? "true" : "false")
       << ",\"cancelled\":" << (m_captureCancelled ? "true" : "false")
       << ",\"done\":" << (m_capturing ? "false" : "true") << "}";
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
                                          const std::wstring& label) {
    // Saving is a separate COM entry point, so do not trust the UI to call it
    // only after a completed capture.  A fresh, successful PAD proof is
    // consumed by exactly one save.
    if (m_capturing.load(std::memory_order_acquire) ||
        m_livenessChecking.load() || !m_livenessPassed.load()) {
        FACELOGIN_ERROR(L"Enrollment save refused: live capture has not completed successfully");
        return false;
    }
    if (m_embeddings.empty()) { FACELOGIN_ERROR(L"No face samples"); return false; }

    // Group samples by capture angle. Capture is sequential, so the flat
    // m_embeddings vector is already grouped in angle order. The group keeps
    // its angle INDEX (not just the label) so the slot's V5 nominal pose
    // (kAngleTargets) can be written with the embedding.
    struct AngleGroup { size_t begin; size_t count; int angle; const wchar_t* label; };
    static constexpr const wchar_t* kAngleLabels[3] = {L"正面", L"左转", L"右转"};
    std::vector<AngleGroup> groups;
    size_t begin = 0;
    for (int a = 0; a < 3; a++) {
        const int count = m_angleSampleCounts[a].load();
        if (count != 0 && count != kAngleTargetFrames) {
            FACELOGIN_ERROR(L"Enrollment save refused: angle %d has %d/%d samples",
                            a, count, kAngleTargetFrames);
            return false;
        }
        if (count > 0)
            groups.push_back({begin, static_cast<size_t>(count), a, kAngleLabels[a]});
        begin += static_cast<size_t>(count);
    }
    if (groups.empty()) { FACELOGIN_ERROR(L"No face samples"); return false; }
    if (begin != m_embeddings.size()) {
        FACELOGIN_ERROR(L"Enrollment save refused: sample metadata/vector mismatch (%zu != %zu)",
                        begin, m_embeddings.size());
        return false;
    }
    const bool multiAngle = groups.size() > 1;

    // All samples share one dimensionality (enrollment uses one recognizer).
    size_t dim = static_cast<size_t>(m_embeddings[0].size());

    m_store.LoadDatabase();

    // --- Slot pre-check (bug4 fix) ---
    // Two-level cap: kMaxUsers accounts total, kMaxFacesPerUser faces per account.
    // Multi-angle re-enrollment REPLACES old faces (not appends) so it never
    // overflows per-account; single-angle append and new-account creation are
    // checked below.
    size_t existingIdx = m_store.FindUserIndex(m_identity.sid, m_identity.upn, m_identity.username);
    const bool accountExists = existingIdx < m_store.GetUsers().size();
    if (accountExists) {
        if (multiAngle) {
            // Multi-angle re-enrollment: clear old faces, keep account identity.
            if (!m_store.ClearFacesForAccount(m_identity.sid)) {
                FACELOGIN_ERROR(L"Failed to clear existing faces for multi-angle re-enrollment");
                return false;
            }
            // After clearing, the account still exists in m_users (identity +
            // password intact, faces empty). The loop below will find it via
            // FindUserIndex → all angles use the append branch, which preserves
            // the stored password. End result: old faces replaced, account
            // identity untouched.
        } else {
            // Single-angle append: check per-account face slots.
            size_t currentFaces = m_store.GetUsers()[existingIdx].faces.size();
            if (currentFaces + groups.size() > facelogin::kMaxFacesPerUser) {
                FACELOGIN_ERROR(L"Not enough face slots: %s has %zu face(s), "
                               L"need %zu more (max %zu per account). "
                               L"Delete unused faces or re-enroll with multi-angle to replace all.",
                               m_identity.username.c_str(), currentFaces, groups.size(),
                               facelogin::kMaxFacesPerUser);
                return false;
            }
        }
    } else {
        // New account: check global user cap.
        if (m_store.GetUsers().size() >= facelogin::kMaxUsers) {
            FACELOGIN_ERROR(L"Cannot create new account: database has %zu users (max %zu). "
                           L"Delete an unused account first.",
                           m_store.GetUsers().size(), facelogin::kMaxUsers);
            return false;
        }
    }

    // Consistency + averaging are done PER ANGLE GROUP — cross-angle samples
    // are NEVER averaged (the embedding space is pose-sensitive; a cross-angle
    // average drifts toward the match boundary, see docs/side-face-plan-v2).
    for (size_t gi = 0; gi < groups.size(); gi++) {
        const AngleGroup& g = groups[gi];

        // Embedding consistency check: verify all samples in this angle group
        // are from the same person. Compute average pairwise distance — if it
        // exceeds the cap, reject.
        // SFace trial uses the bounded 1.00 consistency cap for each angle.
        // It is independent of cross-angle registration and does not average poses.
        {
            double totalDist = 0.0;
            int pairs = 0;
            for (size_t i = g.begin; i < g.begin + g.count; i++) {
                for (size_t j = i + 1; j < g.begin + g.count; j++) {
                    double sum = 0.0;
                    for (size_t k = 0; k < dim; k++) {
                        double diff = m_embeddings[i][k] - m_embeddings[j][k];
                        sum += diff * diff;
                    }
                    totalDist += std::sqrt(sum);
                    pairs++;
                }
            }
            double avgPairDist = (pairs > 0) ? totalDist / pairs : 0.0;
            float maxAllowed = EmbeddingThresholdForDim(kDefaultMatchThreshold, dim);
            FACELOGIN_INFO(L"Enrollment consistency [%ls]: avg pairwise dist=%.4f (max=%.3f, %d pairs, %zu-D)",
                          g.label, avgPairDist, maxAllowed, pairs, dim);
            if (avgPairDist > maxAllowed) {
                FACELOGIN_ERROR(L"Enrollment consistency check failed [%ls]: avg pairwise dist %.4f > %.3f. "
                               L"Samples may be from different faces.", g.label, avgPairDist, maxAllowed);
                return false;
            }
        }

        // Average this group's samples. Initialize to the samples'
        // dimensionality — the vectors require matching sizes.
        std::vector<float> avgEmbedding(dim, 0.0f);
        for (size_t i = g.begin; i < g.begin + g.count; i++) {
            const auto& emb = m_embeddings[i];
            if (emb.size() != avgEmbedding.size()) continue;  // defensive
            for (size_t k = 0; k < dim; k++) avgEmbedding[k] += emb[k];
        }
        for (float& v : avgEmbedding) v /= static_cast<float>(g.count);
        double squaredNorm = 0.0;
        for (float v : avgEmbedding) squaredNorm += static_cast<double>(v) * v;
        const double averageNorm = std::sqrt(squaredNorm);
        if (!std::isfinite(averageNorm) || averageNorm <= 1e-8) return false;
        for (float& v : avgEmbedding) v = static_cast<float>(v / averageNorm);

        // Full dimensionality — 128-D SFace for ONNX. Never truncate.
        std::vector<float> ef = std::move(avgEmbedding);

        // create-or-append (1.3.0): the same account may hold several faces.
        // First-time enrollment stores the (protected) password and face #1;
        // subsequent enrollments APPEND a face and leave the stored password
        // untouched (the user is the logged-on session owner, already trusted).
        // In multi-angle mode each angle is one AddFace: the create branch
        // runs for angle 0, the append branch for angles 1-2.
        // Label: multi-angle always uses the angle name. Single-angle keeps a
        // user-provided name, falling back to the angle name (正面/左转/右转)
        // so an appended face is not labeled with the generic 脸N.
        std::wstring groupLabel = label;
        if (multiAngle || groupLabel.empty()) groupLabel = g.label;
        size_t idx = m_store.FindUserIndex(m_identity.sid, m_identity.upn, m_identity.username);
        uint32_t newFaceId = 0;
        if (idx >= m_store.GetUsers().size()) {
            // First face for this account — protect the password now.
            if (password.empty()) {
                FACELOGIN_ERROR(L"Enrollment refused: a Windows account password is required");
                return false;
            }
            std::vector<uint8_t> protectedPassword = DpapiUtil::Protect(
                reinterpret_cast<const uint8_t*>(password.c_str()),
                static_cast<UINT>(password.size() * sizeof(wchar_t)));
            if (protectedPassword.empty()) { FACELOGIN_ERROR(L"DPAPI encryption failed"); return false; }
            if (!m_store.AddFace(m_identity.username, m_identity.upn, m_identity.sid, protectedPassword, ef,
                                 groupLabel, &newFaceId,
                                 static_cast<float>(kAngleTargets[g.angle]), 0.0f)) {
                FACELOGIN_ERROR(L"Failed to create enrollment for %s", m_identity.username.c_str());
                return false;
            }
        } else {
            // Append a face to an existing account. AddFace ignores the password
            // argument here, so the stored password is preserved.
            if (m_store.GetUsers()[idx].faces.size() >= facelogin::kMaxFacesPerUser) {
                FACELOGIN_ERROR(L"Cannot append: %s already has %zu faces (max %zu)",
                                m_identity.username.c_str(), m_store.GetUsers()[idx].faces.size(),
                                facelogin::kMaxFacesPerUser);
                return false;
            }
            if (!m_store.AddFace(m_identity.username, m_identity.upn, m_identity.sid, {}, ef, groupLabel,
                                 &newFaceId,
                                 static_cast<float>(kAngleTargets[g.angle]), 0.0f)) {
                FACELOGIN_ERROR(L"Failed to append face for %s", m_identity.username.c_str());
                return false;
            }
        }
        FACELOGIN_INFO(L"Enrollment saved for: %s (face #%u, emb=%zu-D, angle=%ls, "
                       L"nominalYaw=%d)",
                       m_identity.username.c_str(), newFaceId, ef.size(), g.label,
                       kAngleTargets[g.angle]);
    }

    if (!m_store.SaveDatabase()) { FACELOGIN_ERROR(L"Failed to save database"); return false; }

    // Cross-angle pair-spacing observation (soft hint, docs/
    // progressive-learning-v2.md red line 3): the learned minimum cross-angle
    // same-person pair distance is 0.681. Angles recorded closer than 0.68
    // mean the user under-turned — the slots overlap, coverage shrinks, and
    // the landing-clarity gate will reject many learning frames. Hint, never
    // block: the enrollment itself is valid.
    m_lastEnrollHint.clear();
    if (groups.size() > 1) {
        size_t idx = m_store.FindUserIndex(m_identity.sid, m_identity.upn, m_identity.username);
        if (idx < m_store.GetUsers().size()) {
            const auto& faces = m_store.GetUsers()[idx].faces;
            float minPair = -1.0f;
            for (size_t i = 0; i < faces.size(); i++) {
                for (size_t j = i + 1; j < faces.size(); j++) {
                    if (faces[i].embedding.size() != faces[j].embedding.size() ||
                        faces[i].embedding.empty()) continue;
                    double sum = 0.0;
                    for (size_t k = 0; k < faces[i].embedding.size(); k++) {
                        double diff = faces[i].embedding[k] - faces[j].embedding[k];
                        sum += diff * diff;
                    }
                    const float d = static_cast<float>(std::sqrt(sum));
                    if (minPair < 0.0f || d < minPair) minPair = d;
                }
            }
            if (minPair >= 0.0f &&
                minPair < facelogin::CredentialStore::kCrossAngleSentinelDist) {
                char buf[192];
                std::snprintf(buf, ARRAYSIZE(buf),
                              "角度提示：各角度模板间距 %.2f（<0.68）。"
                              "下次重录时转角再大一点可提升多角度解锁覆盖"
                              "（本次录入仍然有效）。",
                              minPair);
                m_lastEnrollHint = buf;
                FACELOGIN_WARN(L"Enrollment pair spacing %.3f < %.2f — soft hint shown "
                               L"(learning continues; clarity gate will filter overlap)",
                               minPair,
                               facelogin::CredentialStore::kCrossAngleSentinelDist);
            }
        }
    }

    const bool reloadConfirmed = m_serviceClient.ReloadDatabase();

    // Capture buffer consumed — reset it so the next capture starts clean.
    // Without this, a re-capture of the same angle would see the leftover
    // sample count and complete instantly with zero new frames (bug4 test).
    m_embeddings.clear();
    m_angleSampleCounts[0] = m_angleSampleCounts[1] = m_angleSampleCounts[2] = 0;
    m_livenessPassed = false;
    m_livenessChecking = false;
    return reloadConfirmed;
}


// ============================================================================
// Multi-face management (1.3.0)
// ============================================================================

int EnrollmentWizard::GetFaceCount() {
    m_store.LoadDatabase();
    return static_cast<int>(m_store.GetFaceCount(m_identity.sid));
}

std::string EnrollmentWizard::GetFacesJson() {
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(m_identity.sid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) return "[]";

    std::ostringstream js;
    js << "[";
    const auto& faces = m_store.GetUsers()[idx].faces;
    for (size_t i = 0; i < faces.size(); i++) {
        if (i > 0) js << ",";
        const auto& f = faces[i];
        js << "{\"id\":" << f.id
           << ",\"label\":\"" << WstrToUtf8Escaped(f.label) << "\"}";
    }
    js << "]";
    return js.str();
}

bool EnrollmentWizard::SaveEnrollmentAppend(const std::wstring& label) {
    // The appended face belongs to the logged-on session owner — the session
    // token SID must match the enrolled account. No password is required for
    // an append because the stored password is retained.
    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    if (tokenSid.empty() || tokenSid != m_identity.sid) {
        FACELOGIN_ERROR(L"Face append refused: token SID %s != enrolled SID %s",
                        tokenSid.c_str(), m_identity.sid.c_str());
        return false;
    }
    return SaveEnrollmentImpl(L"", label);
}

bool EnrollmentWizard::DeleteFace(int faceId) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    if (!m_store.DeleteFace(m_identity.sid, static_cast<uint32_t>(faceId))) return false;
    if (!m_store.SaveDatabase()) return false;
    return m_serviceClient.ReloadDatabase();
}

bool EnrollmentWizard::ClearAllFaces() {
    m_store.LoadDatabase();
    if (!m_store.ClearAllFaces(m_identity.sid)) return false;
    if (!m_store.SaveDatabase()) return false;
    return m_serviceClient.ReloadDatabase();
}

bool EnrollmentWizard::RenameFace(int faceId, const std::wstring& label) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    if (!m_store.RenameFace(m_identity.sid, static_cast<uint32_t>(faceId), label)) return false;
    if (!m_store.SaveDatabase()) return false;
    return m_serviceClient.ReloadDatabase();
}

// Detect a stale account-type record (symmetric MSA ↔ local). We never trust
// m_identity.upn/m_identity.accountType here — they are derived from the session at construction
// and could be empty for local accounts; instead we re-query the CURRENT
// session identity with session_identity::ResolveSessionUpn() (docs/todo.md bug1):
//   UPN contains '@'  → current account is MSA
//   empty (after fallback)  → current account is local
// Then compare against the stored record (matched by SID, which Windows keeps
// across MSA↔local conversions):
//   local + record UPN is an MSA email  → state 1 (MSA→local, clear UPN)
//   MSA   + record UPN empty/different  → state 2 (local→MSA, write current UPN)
//   everything else                     → state 0 (no refresh needed)
int EnrollmentWizard::GetAccountTypeChanged() {
    // Determine the CURRENT session's account type via the authoritative
    // query used everywhere else (docs/todo.md bug1). Empty (after fallback)
    // means no MSA identity → local account.
    std::wstring curUpn = session_identity::ResolveSessionUpn();
    bool sessionIsMsa = session_identity::IsMsaUpn(curUpn);

    // Match the current identity against stored records (same priority as
    // FindUserIndex: SID > UPN > username).
    m_store.LoadDatabase();
    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    size_t idx = m_store.FindUserIndex(tokenSid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) return 0;  // not enrolled → normal first-time flow

    const auto& rec = m_store.GetUsers()[idx];
    if (!sessionIsMsa) {
        // Current account is local. Flag if the record still carries an MSA email.
        if (rec.upn.find(L'@') != std::wstring::npos) {
            FACELOGIN_INFO(L"GetAccountTypeChanged: stale MSA→local record for %s (UPN=%s, faces=%zu)",
                           rec.username.c_str(), rec.upn.c_str(), rec.faces.size());
            return 1;
        }
        return 0;
    }

    // Current account is MSA. Flag if the record UPN is empty (local-era) or a
    // different email than the current session's MSA identity.
    if (rec.upn.empty() || rec.upn != curUpn) {
        FACELOGIN_INFO(L"GetAccountTypeChanged: stale local→MSA record for %s (stored UPN=%s, current=%s, faces=%zu)",
                       rec.username.c_str(), rec.upn.empty() ? L"<empty>" : rec.upn.c_str(),
                       curUpn.c_str(), rec.faces.size());
        return 2;
    }
    return 0;
}

std::string EnrollmentWizard::CheckAccountTypeChanged() {
    int state = GetAccountTypeChanged();
    if (state == 0) return "{\"state\":0}";

    // Attach the face count so the prompt can say "你已录入 N 张人脸", and for
    // state 2 the current MSA email so the prompt can show what will be written.
    m_store.LoadDatabase();
    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    size_t idx = m_store.FindUserIndex(tokenSid, m_identity.upn, m_identity.username);
    size_t faces = (idx < m_store.GetUsers().size()) ? m_store.GetUsers()[idx].faces.size() : 0;

    if (state == 2) {
        // Re-derive the current MSA email (same authoritative query).
        std::wstring curUpn = session_identity::ResolveSessionUpn();
        return "{\"state\":2,\"faces\":" + std::to_string(faces) +
               ",\"upn\":\"" + WstrToUtf8Escaped(curUpn) + "\"}";
    }
    return "{\"state\":1,\"faces\":" + std::to_string(faces) + "}";
}

bool EnrollmentWizard::RefreshAccountIdentity(const std::wstring& password) {
    int state = GetAccountTypeChanged();
    if (state == 0) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: no stale record to refresh");
        return false;
    }
    if (password.empty()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: empty password");
        return false;
    }

    // Validate the CURRENT password before touching the database.
    // ValidatePassword routes by account type: MSA (UPN contains '@') goes
    // through CloudAP online/cached validation, local uses LogonUserW(".").
    if (!ValidatePassword(password)) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: password validation failed");
        return false;
    }

    // Re-encrypt with DPAPI (machine scope — matches first enrollment).
    std::vector<uint8_t> protectedPassword = DpapiUtil::Protect(password);
    if (protectedPassword.empty()) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: DPAPI encryption failed");
        return false;
    }

    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(tokenSid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: record vanished before update");
        return false;
    }

    // Write the identity matching the CURRENT account type:
    //   state 1 (MSA→local): clear the old MSA UPN.
    //   state 2 (local→MSA): write the current MSA email (session UPN).
    std::wstring newUpn;
    if (state == 2) {
        newUpn = session_identity::ResolveSessionUpn();
    }
    // state 1 → newUpn stays empty (local account).

    if (!m_store.UpdateAccountIdentity(idx, m_identity.username, newUpn, tokenSid, protectedPassword)) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: identity update failed");
        return false;
    }
    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: save failed");
        return false;
    }

    const bool reloadConfirmed = m_serviceClient.ReloadDatabase();
    FACELOGIN_INFO(L"RefreshAccountIdentity: refreshed identity of %s (UPN=%s%s, faces preserved)",
                   m_identity.username.c_str(),
                   newUpn.empty() ? L"<cleared>" : newUpn.c_str(),
                   state == 2 ? L", MSA" : L", local");
    return reloadConfirmed;
}

// Dismiss path for the stale-account prompt. Unlike RefreshAccountIdentity
// this needs NO user input: it only drops the '@'-carrying UPN that a buggy
// build wrote into a LOCAL account's record (docs/todo.md bug1), keeping
// username, SID, faces and the DPAPI-encrypted password byte-for-byte intact.
//   - state 1 (current session local + record carries an MSA email): the
//     email is either a misattribution (another account / converted MSA) —
//     clearing it makes the lock-screen pack domain\username, fixing the
//     silent login failure — or a genuine MSA→local conversion where the user
//     simply doesn't want to re-enter the password; identity is corrected and
//     if the password changed they should still run the full refresh.
//   - state 2 (local→MSA) is deliberately NOT touched: the record needs the
//     CURRENT email + re-encrypted password, which only the refresh provides.
bool EnrollmentWizard::ClearStaleAccountUpn() {
    int state = GetAccountTypeChanged();
    if (state != 1) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: not a stale MSA→local record (state=%d), no-op", state);
        return false;
    }

    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(tokenSid, m_identity.upn, m_identity.username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"ClearStaleAccountUpn: record vanished before update");
        return false;
    }
    const auto& rec = m_store.GetUsers()[idx];
    if (rec.upn.find(L'@') == std::wstring::npos) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: record already has no MSA email, no-op");
        return false;
    }

    // Pass the record's own encryptedPassword back unchanged so the stored
    // credential is not re-encrypted (only the identity email is dropped).
    if (!m_store.UpdateAccountIdentity(idx, rec.username, L"", tokenSid, rec.encryptedPassword)) {
        FACELOGIN_ERROR(L"ClearStaleAccountUpn: identity update failed");
        return false;
    }
    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"ClearStaleAccountUpn: save failed");
        return false;
    }

    const bool reloadConfirmed = m_serviceClient.ReloadDatabase();
    FACELOGIN_INFO(L"ClearStaleAccountUpn: cleared stale MSA UPN for %s (faces=%zu, password untouched)",
                   rec.username.c_str(), rec.faces.size());
    return reloadConfirmed;
}

bool EnrollmentWizard::AutoRepairEmptyUpnOnStartup() {
    // Re-derive the current session's MSA identity (GetUserNameExW + token
    // fallback). Only proceed when we actually have an MSA email to write.
    std::wstring curUpn = session_identity::ResolveSessionUpn();
    if (!session_identity::IsMsaUpn(curUpn)) {
        // Local session (or detection inconclusive) — nothing to repair.
        return false;
    }

    std::wstring tokenSid = session_identity::GetCurrentProcessUserSid();
    if (tokenSid.empty()) {
        FACELOGIN_WARN(L"AutoRepairEmptyUpnOnStartup: could not resolve current session SID, skipping");
        return false;
    }

    m_store.LoadDatabase();
    bool modified = false;
    for (size_t idx = 0; idx < m_store.GetUsers().size(); ++idx) {
        const auto& rec = m_store.GetUsers()[idx];
        // Only THIS session user's own record, and only when its UPN is empty.
        // A non-empty but different UPN may be a genuine re-binding and is left
        // to RefreshAccountIdentity (which requires a password check).
        if (rec.sid != tokenSid) continue;
        if (!rec.upn.empty()) continue;

        // Pass rec.sid (not tokenSid) and the existing encryptedPassword back
        // unchanged so the record stays self-consistent and the stored
        // credential is not re-encrypted; faces are preserved by UpdateAccountIdentity.
        if (!m_store.UpdateAccountIdentity(idx, rec.username, curUpn, rec.sid, rec.encryptedPassword)) {
            FACELOGIN_ERROR(L"AutoRepairEmptyUpnOnStartup: identity update failed for %s", rec.username.c_str());
            continue;
        }
        FACELOGIN_INFO(L"AutoRepairEmptyUpnOnStartup: repaired empty UPN for %s (sid=%s, new upn=%s, faces=%zu, password untouched)",
                       rec.username.c_str(), rec.sid.c_str(), curUpn.c_str(), rec.faces.size());
        modified = true;
    }

    if (!modified) return false;

    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"AutoRepairEmptyUpnOnStartup: save failed");
        return false;
    }
    return m_serviceClient.ReloadDatabase();
}

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
