#pragma once

#include "enrollment_samples.h"
#include "../common/frame_image.h"
#include "../vision/onnx_models.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace facelogin {

struct EnrollmentCaptureStatus {
    int angle = 0;
    int collected = 0;
    int total = 0;
    int targetYaw = 0;
    float yaw = 0;
    const wchar_t* label = L"正面";
    bool checking = false;
    bool passed = false;
    bool cancelled = false;
    bool done = true;
};

// Owns the sampling thread, progress and unsaved samples. Control methods run
// on the UI thread; status getters may also run on the preview thread. Models
// and the frame reader must remain alive until Stop() returns. The reader
// copies a preview frame and its sequence under the host's frame-cache mutex;
// the incoming sequence is the last consumed one, allowing duplicate rejection
// before copying the pixels.
class EnrollmentCapture {
public:
    using FrameReader = std::function<bool(FrameImage&, std::uint64_t&)>;
    static constexpr int kAngleTargetFrames = 5;

    ~EnrollmentCapture();
    bool Start(int angle, OnnxDetector& detector, OnnxRecognizer& recognizer,
               OnnxAntiSpoof& antiSpoof, float threshold, FrameReader readFrame);
    void Cancel();
    void Stop();
    bool IsRunning() const { return m_running.load(); }
    int SampleCount() const { return m_samplesCollected.load(); }
    bool LivenessPassed() const { return m_livenessPassed.load(); }
    bool LivenessChecking() const { return m_livenessChecking.load(); }
    EnrollmentCaptureStatus Status() const;

    // Returns an independent, validated snapshot only after the sampling
    // thread has finished. A failed database save may retry the same proof;
    // a successful save must consume it through ClearSavedSamples().
    bool CopyCompletedSamples(EnrollmentSamples& out);
    void ClearSavedSamples();

private:
    void Run(int angleIndex, OnnxDetector& detector, OnnxRecognizer& recognizer,
             OnnxAntiSpoof& antiSpoof, float threshold, const FrameReader& readFrame);

    static constexpr int kAngleTargets[3] = {0, 30, -30};
    static constexpr const wchar_t* kAngleLabels[3] = {L"正面", L"左转", L"右转"};
    std::thread m_captureThread;
    // Stop request and actual thread completion are separate: cancellation
    // must not publish the vector or permit exposure changes while Run exits.
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_captureCancelled{false};
    std::atomic<bool> m_livenessPassed{false};
    std::atomic<bool> m_livenessChecking{false};
    std::atomic<int> m_samplesCollected{0};
    std::atomic<int> m_angleSampleCounts[3] = {0, 0, 0};
    std::atomic<int> m_captureAngle{0};
    std::atomic<float> m_lastYaw{0};
    std::vector<std::vector<float>> m_embeddings;
};

} // namespace facelogin
