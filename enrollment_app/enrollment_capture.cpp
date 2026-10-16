#include "enrollment_capture.h"

#include "../common/logger.h"
#include "../vision/face_align.h"
#include "../vision/liveness_types.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

namespace facelogin {

EnrollmentCapture::~EnrollmentCapture() { Stop(); }

bool EnrollmentCapture::Start(int angleIndex, OnnxDetector& detector,
                              OnnxRecognizer& recognizer, OnnxAntiSpoof& antiSpoof,
                              float threshold, FrameReader readFrame) {
    if (angleIndex < 0 || angleIndex > 2 || !readFrame ||
        !detector.IsInitialized() || !recognizer.IsInitialized() || !antiSpoof.IsInitialized()) {
        FACELOGIN_ERROR(L"Enrollment capture refused: invalid angle or unavailable input/models");
        return false;
    }
    // A repeated UI/COM request must not block waiting for fresh camera frames
    // or for the user to turn. Only join a worker that has already finished.
    if (IsRunning()) {
        FACELOGIN_WARN(L"Enrollment capture is already running");
        return false;
    }
    if (m_captureThread.joinable()) {
        m_captureThread.join();
        m_capturing = false;
    }

    m_capturing = true;
    m_running = true;
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
        // Trimming drops this angle and all later angles, so their metadata
        // must be reset together with the vector.
        for (int a = angleIndex; a < 3; ++a) m_angleSampleCounts[a] = 0;
        m_samplesCollected = static_cast<int>(prior);
    }
    m_livenessPassed = false;
    m_livenessChecking = true;

    try {
        m_captureThread = std::thread([this, angleIndex, &detector, &recognizer,
                                       &antiSpoof, threshold, readFrame = std::move(readFrame)] {
            // Publish completion on every exit, including inference errors.
            struct Completion {
                EnrollmentCapture& capture;
                ~Completion() {
                    capture.m_livenessChecking = false;
                    capture.m_capturing = false;
                    capture.m_running.store(false, std::memory_order_release);
                }
            } completion{*this};
            try {
                Run(angleIndex, detector, recognizer, antiSpoof, threshold, readFrame);
            } catch (const std::exception& error) {
                m_livenessPassed = false;
                FACELOGIN_ERROR(L"Enrollment capture failed: %hs", error.what());
            }
        });
    } catch (const std::exception& error) {
        m_livenessChecking = false;
        m_capturing = false;
        m_running = false;
        FACELOGIN_ERROR(L"Enrollment capture thread start failed: %hs", error.what());
        return false;
    }
    return true;
}

void EnrollmentCapture::Run(int angleIndex, OnnxDetector& detector,
                            OnnxRecognizer& recognizer, OnnxAntiSpoof& antiSpoof,
                            float threshold, const FrameReader& readFrame) {
    // Phase 1: every independently callable capture must prove liveness.
    // In particular, side-angle append is exposed through COM and cannot
    // inherit a stale proof from an earlier front capture or save.
    bool livenessPassed = false;
    bool livenessInferenceError = false;
    std::uint64_t lastFrameSequence = 0;

    // The preview and capture threads run independently. Consume each
    // cached camera frame at most once so a stalled preview cannot turn one
    // PAD result into a synthetic 5/5 sequence.
    const auto copyFreshFrame = [&readFrame, &lastFrameSequence](FrameImage& output) {
        std::uint64_t sequence = lastFrameSequence;
        if (!readFrame(output, sequence) || sequence == lastFrameSequence) return false;
        lastFrameSequence = sequence;
        return true;
    };

    FACELOGIN_INFO(L"Enrollment: starting mandatory anti-spoof check for angle %d",
                   angleIndex);

    if (antiSpoof.IsInitialized()) {
        // Enrollment keeps the calibrated five-frame gate regardless of
        // the fast_unlock preference: that switch trades liveness
        // evidence for speed at unlock time only.
        int totalChecks = AntiSpoofCheckCount(/*fastUnlock=*/false);
        int passRequired = AntiSpoofPassRequired(totalChecks);
        FACELOGIN_INFO(L"Enrollment anti-spoof: threshold=%.3f → %d checks, %d required",
                       threshold, totalChecks, passRequired);
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
            auto asDet = detector.DetectLargestFace(frame);
            if (!asDet) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }

            float score = antiSpoof.Predict(frame,
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
            if (score >= threshold) passCount++; // config-driven threshold
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
        auto onnxDet = detector.DetectLargestFace(frame);
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

        const float samplePadScore = antiSpoof.Predict(frame,
            FaceRect(static_cast<long>(onnxDet->x1),
                            static_cast<long>(onnxDet->y1),
                            static_cast<long>(onnxDet->x2),
                            static_cast<long>(onnxDet->y2)));
        if (!std::isfinite(samplePadScore) || samplePadScore < 0.0f ||
            samplePadScore > 1.0f || samplePadScore < threshold) {
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
        auto onnxEmb = recognizer.ComputeEmbedding(frame, onnxDet->kps,
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
    m_capturing = false;
}

void EnrollmentCapture::Cancel() {
    if (IsRunning()) {
        FACELOGIN_INFO(L"Enrollment: capture cancelled by user");
        m_captureCancelled = true;
        m_capturing = false;
    }
}

void EnrollmentCapture::Stop() {
    m_capturing = false;
    if (m_captureThread.joinable()) m_captureThread.join();
}

EnrollmentCaptureStatus EnrollmentCapture::Status() const {
    EnrollmentCaptureStatus status;
    status.done = !IsRunning(); // acquire completed progress before reading it
    status.angle = m_captureAngle.load();
    status.collected = m_angleSampleCounts[status.angle].load();
    for (const auto& count : m_angleSampleCounts) status.total += count.load();
    status.label = kAngleLabels[status.angle];
    status.targetYaw = kAngleTargets[status.angle];
    status.yaw = m_lastYaw.load();
    status.checking = m_livenessChecking.load();
    status.passed = m_livenessPassed.load();
    status.cancelled = m_captureCancelled.load();
    return status;
}

bool EnrollmentCapture::CopyCompletedSamples(EnrollmentSamples& out) {
    out = {};
    if (IsRunning() || m_livenessChecking || !m_livenessPassed || m_captureCancelled) {
        FACELOGIN_ERROR(L"Enrollment save refused: live capture has not completed successfully");
        return false;
    }
    // Joining is non-blocking apart from thread teardown once running=false.
    // Keep this ownership guarantee inside the module, away from persistence.
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_embeddings.empty()) { FACELOGIN_ERROR(L"No face samples"); return false; }
    EnrollmentSamples completed;
    size_t begin = 0;
    for (int angle = 0; angle < 3; ++angle) {
        const int count = m_angleSampleCounts[angle].load();
        if (count != 0 && count != kAngleTargetFrames) {
            FACELOGIN_ERROR(L"Enrollment save refused: angle %d has %d/%d samples",
                            angle, count, kAngleTargetFrames);
            return false;
        }
        if (count > 0) completed.groups.push_back({begin, static_cast<size_t>(count),
            angle, kAngleLabels[angle], kAngleTargets[angle]});
        begin += static_cast<size_t>(count);
    }
    if (completed.groups.empty() || begin != m_embeddings.size()) {
        FACELOGIN_ERROR(L"Enrollment save refused: sample metadata/vector mismatch (%zu != %zu)",
                        begin, m_embeddings.size());
        return false;
    }
    completed.embeddings = m_embeddings;
    out = std::move(completed);
    return true;
}

void EnrollmentCapture::ClearSavedSamples() {
    // Called by the UI thread only after CopyCompletedSamples and a successful save.
    if (IsRunning()) return;
    if (m_captureThread.joinable()) m_captureThread.join();
    m_embeddings.clear();
    m_angleSampleCounts[0] = m_angleSampleCounts[1] = m_angleSampleCounts[2] = 0;
    m_livenessPassed = false;
    m_livenessChecking = false;
}

} // namespace facelogin
