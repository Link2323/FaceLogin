#pragma once

#include <chrono>

namespace facelogin {

// Calibration and validation use independent five-frame attempts. The
// default (secure) policy mirrors that exactly: five valid frames, and
// every frame must pass. Fast unlock (config fast_unlock=true) counts
// three frames with every frame binding identity — a user-facing
// speed/security trade-off, NOT a calibration result (docs/
// threshold-calibration.md is five-frame; the 3-frame mode was not
// recalibrated and keeps the same per-frame 0.281 threshold).
inline int AntiSpoofCheckCount(bool fastUnlock) {
    return fastUnlock ? 3 : 5;
}

inline int AntiSpoofPassRequired(int checkCount) {
    return checkCount;
}

// Pure timing gate for the anti-spoof loop's fast-fail decisions, kept free
// of models, cameras and wall-clock reads so the semantics stay
// unit-testable:
//   - the 2.5 s empty-scene timer times from the PAD window start;
//   - the 2 s persistent-attack timer times from the FIRST detected face,
//     so a user who steps into the frame late still reaches PAD inference
//     instead of an instant attack verdict;
//   - the attack fast fail applies only before the first PAD pass — after a
//     pass, all-fail sequences are bounded by the every-frame-pass rule and
//     the 8 s window, never by a second fast-fail timer.
struct LivenessTiming {
    using TimePoint = std::chrono::steady_clock::time_point;

    static constexpr double kEmptySceneSeconds = 2.5;
    static constexpr double kAttackSeconds = 2.0;
    static constexpr double kWindowSeconds = 8.0;

    explicit LivenessTiming(TimePoint windowStart)
        : m_windowStart(windowStart), m_noFaceStart(windowStart) {}

    // Per-iteration check before grabbing a frame.
    bool WindowExpired(TimePoint now) const {
        return Elapsed(now, m_windowStart) >= kWindowSeconds;
    }

    // The scene has stayed empty: no face has ever been detected.
    bool ShouldFailEmptyScene(TimePoint now) const {
        return !m_anyFaceSeen && Elapsed(now, m_noFaceStart) >= kEmptySceneSeconds;
    }

    // Call after a frame with a detected face.
    void OnFaceDetected(TimePoint now) {
        if (!m_anyFaceSeen) {
            m_anyFaceSeen = true;
            m_firstFaceSeenAt = now;
        }
    }

    // Bracket a pre-binding exposure correction (auth_pipeline.cpp) with
    // Suspend/Resume: sensor settling is capture preparation, not liveness
    // evaluation, so the paused span is subtracted by shifting every timer
    // origin forward. Without this, a bounded tune that starts before the
    // first counted frame burns the 2.5 s empty-scene budget (and, once a
    // face has been seen, the 2 s attack budget) while the loop is not even
    // evaluating frames. The 8 s PAD window pauses with them.
    void Suspend(TimePoint now) {
        m_suspendedAt = now;
        m_suspended = true;
    }
    void Resume(TimePoint now) {
        if (!m_suspended) return;
        m_suspended = false;
        const auto shift = now - m_suspendedAt;
        m_windowStart += shift;
        m_noFaceStart += shift;
        // Unread while !m_anyFaceSeen; shifting it anyway keeps one rule.
        m_firstFaceSeenAt += shift;
    }

    // Call after a frame with a detected face; must never fire on the frame
    // that first sees the face (that face still gets a PAD inference).
    bool ShouldFailPersistentAttack(TimePoint now) const {
        return m_anyFaceSeen && m_passCount == 0 &&
               Elapsed(now, m_firstFaceSeenAt) >= kAttackSeconds;
    }

    void OnPadScore(bool passed) {
        if (passed) ++m_passCount;
    }

    bool AnyFaceSeen() const { return m_anyFaceSeen; }
    int PassCount() const { return m_passCount; }

private:
    static double Elapsed(TimePoint now, TimePoint since) {
        return std::chrono::duration<double>(now - since).count();
    }

    TimePoint m_windowStart;
    TimePoint m_noFaceStart;
    bool m_anyFaceSeen = false;
    TimePoint m_firstFaceSeenAt{};
    int m_passCount = 0;
    bool m_suspended = false;
    TimePoint m_suspendedAt{};
};

} // namespace facelogin
