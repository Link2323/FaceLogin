#include "preview_exposure.h"

#include <utility>

namespace facelogin {
namespace {
constexpr std::chrono::seconds kExposureWatchInterval(3);
constexpr std::chrono::seconds kExposureTuneCooldown(10);
} // namespace

PreviewExposure::PreviewExposure(WebcamCaptureDS& camera, std::wstring statePath)
    : m_camera(camera), m_statePath(std::move(statePath)) {}

GainTuneConfig PreviewExposure::TuneConfig() {
    GainTuneConfig config;
    config.persistPath = m_statePath;
    config.recovery = &m_recovery;
    return config;
}

void PreviewExposure::Begin() {
    m_replay = ReplayExposureState(BuildSensorKnobs(), m_statePath);
}

void PreviewExposure::Prepare(OnnxDetector& detector, int rotation) {
    m_recovery = {};
    const auto config = TuneConfig();
    // No pre-replay measurement exists to prove an early response. Model
    // loading absorbs some or all of this existing driver settle interval.
    while (m_replay.wrote && std::chrono::steady_clock::now() - m_replay.startedAt <
                                std::chrono::milliseconds(config.settleMs)) {
        FrameImage discard;
        m_camera.GrabFrame(discard);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TuneFaceExposure(BuildSensorKnobs(),
                     [this](FrameImage& frame, unsigned long long& sequence) {
                         return m_camera.GrabFrame(frame, &sequence);
                     }, detector, [] { return false; }, rotation, config);
    m_lastTune = std::chrono::steady_clock::now();
    m_lastWatch = m_lastTune;
    m_pendingLuma = -1.0f;
}

void PreviewExposure::CameraReopened() {
    // Preserve watch cadence/cooldown; only evidence from the old graph expires.
    m_recovery = {};
}

SensorKnobs PreviewExposure::BuildSensorKnobs() {
    SensorKnobs knobs;
    knobs.exposureRange = [this](long& mn, long& mx, long& st) {
        return m_camera.GetExposureRange(&mn, &mx, &st);
    };
    knobs.exposureGet = [this](long& v) { return m_camera.GetExposure(&v); };
    knobs.exposureSet = [this](long v) { return m_camera.SetExposure(v); };
    knobs.exposureSetAuto = [this](long v) { return m_camera.SetExposure(v, false); };
    knobs.exposureIsManual = [this](bool& manual) {
        long value = 0;
        return m_camera.GetExposure(&value, &manual);
    };
    knobs.gainRange = [this](long& mn, long& mx, long& st) {
        return m_camera.GetGainRange(&mn, &mx, &st);
    };
    knobs.gainGet = [this](long& v) { return m_camera.GetGain(&v); };
    knobs.gainSet = [this](long v) { return m_camera.SetGain(v); };
    return knobs;
}

void PreviewExposure::Observe(const FrameImage& frame,
                              const OnnxDetector::Detection* det,
                              unsigned long long frameSequence, OnnxDetector& detector, int rotation,
                              const std::function<bool()>& paused) {
    // A capture in flight must see one stable capture domain — never move the
    // sensor between its PAD/embedding frames.
    if (paused()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastWatch < kExposureWatchInterval) return;
    m_lastWatch = now;
    auto cfg = TuneConfig();
    if (det && m_recovery.pending) {
        const float luma = FaceBoxLuma(frame, *det);
        if (luma >= cfg.minFaceLuma && luma <= cfg.maxFaceLuma) {
            // Retry on the watch cadence, independently of identity/PAD and
            // the write cooldown. A failed check never refreshes the cache.
            m_recovery.attempted = false;
            ConfirmPendingFaceTune(BuildSensorKnobs(),
                [this](FrameImage& f, unsigned long long& s) { return m_camera.GrabFrame(f, &s); },
                detector, paused,
                rotation, frameSequence, cfg);
            return;
        }
    }
    if (now - m_lastTune < kExposureTuneCooldown) return;
    if (PreferAutoExposureWithoutGain(BuildSensorKnobs())) {
        m_pendingLuma = -1.0f;
        return;
    }
    if (!det) {
        m_pendingLuma = -1.0f;
        if (!IsSceneWhiteout(frame) && !IsSceneDark(frame, cfg)) return;
        m_lastTune = now;
        TuneFaceExposure(BuildSensorKnobs(),
                         [this](FrameImage& f, unsigned long long& s) {
                             return m_camera.GrabFrame(f, &s);
                         }, detector,
                         paused,
                         rotation, cfg);
        return;
    }
    const float luma = FaceBoxLuma(frame, *det);
    const bool outOfBand =
        luma >= 0.0f && (luma < cfg.minFaceLuma || luma > cfg.maxFaceLuma ||
                        detail::CanShortenExposure(luma, BuildSensorKnobs(), cfg));
    if (!outOfBand) {
        m_pendingLuma = -1.0f;
        return;
    }
    // Two consecutive out-of-band cycles (3 s apart): single readings jitter
    // with face fill ratio, and one spurious hit must not move the sensor.
    if (m_pendingLuma < 0.0f ||
        std::fabs(luma - m_pendingLuma) > 40.0f) {
        m_pendingLuma = luma;
        return;
    }
    m_pendingLuma = -1.0f;
    m_lastTune = now;
    FACELOGIN_INFO(L"Preview exposure watch: face luma %.0f (band %.0f–%.0f), correction/shortening "
                   L"(twice) — re-tuning",
                   luma, cfg.minFaceLuma, cfg.maxFaceLuma);
    // Runs inline on the frame thread: the preview image pauses for the tune
    // budget (hundreds of ms) once per drift event — invisible next to a
    // washed-out preview. The trigger luma is this frame's; the tune grabs
    // its own fresh settle frames.
    TuneFaceExposure(luma, BuildSensorKnobs(),
                     [this](FrameImage& f, unsigned long long& s) {
                         return m_camera.GrabFrame(f, &s);
                     },
                     detector, [] { return false; },
                     rotation, cfg);
}

} // namespace facelogin
