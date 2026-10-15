#pragma once

#include "auth_frame.h"
#include <functional>
#include <optional>

namespace facelogin {

// Per-authentication capture pacing. All calls run on the camera's COM thread.
// Rejected frames never advance the anchor. The caller advances it only after
// identity/continuity checks, before prefetching the next frame while PAD runs.
class AuthFrameStream {
public:
    using GrabFrame = std::function<bool(FrameImage&, unsigned long long&)>;
    AuthFrameStream(OnnxDetector& detector, int rotation, GrabFrame grabFrame,
                    std::function<bool()> shouldStop);

    // False means grab failure or interruption; the caller retains terminal
    // state ownership. The stop predicate is rechecked during stale-frame waits.
    bool Grab(AuthFrame& out);
    void AdvanceAnchor(const AuthFrame& frame);
    std::optional<double> MinimumIntervalMs() const { return m_minIntervalMs; }

private:
    OnnxDetector& m_detector;
    int m_rotation;
    GrabFrame m_grabFrame;
    std::function<bool()> m_shouldStop;
    std::chrono::steady_clock::time_point m_lastCountedWall{};
    unsigned long long m_lastCountedSeq = 0;
    bool m_haveLastCounted = false;
    std::optional<double> m_minIntervalMs;
};

} // namespace facelogin
