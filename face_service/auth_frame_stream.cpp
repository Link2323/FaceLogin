#include "auth_frame_stream.h"
#include "../vision/face_align.h"

#include <chrono>
#include <thread>
#include <utility>

namespace facelogin {

AuthFrameStream::AuthFrameStream(OnnxDetector& detector, int rotation,
                                 GrabFrame grabFrame, std::function<bool()> shouldStop)
    : m_detector(detector), m_rotation(rotation), m_grabFrame(std::move(grabFrame)),
      m_shouldStop(std::move(shouldStop)) {}

bool AuthFrameStream::Grab(AuthFrame& out) {
    for (;;) {
        if (m_shouldStop()) return false;
        if (m_haveLastCounted) {
            const auto deadline = m_lastCountedWall + std::chrono::milliseconds(60);
            if (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_until(deadline);
                continue;                   // re-check cancel after waking
            }
        }
        FrameImage frame;
        unsigned long long seq = 0;
        if (!m_grabFrame(frame, seq)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return false;
        }
        // Defensive dedup: the 60 ms guard guarantees a fresh 30 fps
        // frame mathematically, but a stale buffered frame would
        // silently violate the strictly-increasing-sequence invariant.
        if (m_haveLastCounted && seq <= m_lastCountedSeq) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Anchor the guard at the GRAB (acquisition), not after
        // detection: the invariant is acquisition spacing, and a
        // post-detect anchor would serialize each frame's SCRFD run
        // on top of the guard wait (measured 27–50 ms/frame p50–max
        // on the dev machine). Detection of the next frame must fit
        // inside the 60 ms window; where it doesn't (slow machines,
        // ~153 ms on the 1360P) the wait shrinks to zero and
        // detection itself becomes the pacing floor — never worse.
        const auto grabWall = std::chrono::steady_clock::now();
        RotateFrame(frame, m_rotation);
        auto det = m_detector.DetectLargestFace(frame);
        out = AuthFrame{ true, std::move(frame), seq, std::move(det), grabWall };
        return true;
    }
}

void AuthFrameStream::AdvanceAnchor(const AuthFrame& frame) {
    if (m_haveLastCounted) {
        const double intervalMs = std::chrono::duration<double, std::milli>(
            frame.grabWall - m_lastCountedWall).count();
        if (!m_minIntervalMs || intervalMs < *m_minIntervalMs)
            m_minIntervalMs = intervalMs;
    }
    m_lastCountedWall = frame.grabWall;
    m_lastCountedSeq = frame.seq;
    m_haveLastCounted = true;
}

} // namespace facelogin
