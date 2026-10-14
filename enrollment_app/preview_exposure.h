#pragma once

#include "../vision/face_gain_tune.h"
#include "../vision/webcam_capture_dshow.h"

namespace facelogin {

// Owns exposure state for one preview session, but never owns the camera or
// detector. Begin/Prepare run before the frame thread starts; Observe and
// CameraReopened run only on that thread. Destroy after the thread joins.
class PreviewExposure {
public:
    PreviewExposure(WebcamCaptureDS& camera, std::wstring statePath);
    PreviewExposure(const PreviewExposure&) = delete;
    PreviewExposure& operator=(const PreviewExposure&) = delete;

    // Replay before model loading, then use that elapsed time for settling.
    void Begin();
    void Prepare(OnnxDetector& detector, int rotation);
    void Observe(const FrameImage& frame, const OnnxDetector::Detection* detection,
                 unsigned long long sequence, OnnxDetector& detector, int rotation,
                 const std::function<bool()>& paused);
    void CameraReopened();

private:
    SensorKnobs BuildSensorKnobs();
    GainTuneConfig TuneConfig();

    WebcamCaptureDS& m_camera;
    std::wstring m_statePath;
    ExposureReplay m_replay;
    FaceTuneRecovery m_recovery;
    std::chrono::steady_clock::time_point m_lastWatch{};
    std::chrono::steady_clock::time_point m_lastTune{};
    float m_pendingLuma = -1.0f;
};

} // namespace facelogin
