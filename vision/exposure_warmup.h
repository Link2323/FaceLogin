#pragma once

#include <algorithm>
#include <vector>

#include "../common/frame_image.h"

namespace facelogin {

// Adaptive auto-exposure settling gate for the auth loop (idea borrowed from
// upstream FaceLogin 1.7.0, constants re-anchored to this fork's baseline):
// before the first PAD frame, sample the mean luma of DISTINCT camera frames
// and proceed as soon as a rolling window is stable, instead of a fixed
// frame count.
//
// Anchoring, and why these constants differ from upstream's (min 2 / window
// 3 / stable 2): this fork's legacy warmup discarded 3 loop iterations that
// usually completed before the camera delivered its first frame — the
// liveness loop often started on the FIRST distinct frame. Two distinct
// frames within lumaTol is therefore already stricter than what the PAD
// calibration shipped with, while the anti-spoof all-frames-pass rule (not
// this gate) remains the security boundary. Scenes where AGC converges
// slowly (cold start, dark room) still extend the wait up to maxSamples
// instead of proceeding with under-exposed frames, which is the upstream
// root cause for PAD score jitter on real users.
struct ExposureWarmupConfig {
    int minSamples = 2;     // never open the gate before this many samples
    int maxSamples = 10;    // hard cap — proceed regardless of stability
    int window = 2;         // rolling mean-luma window size
    float lumaTol = 20.0f;  // window max-min that counts as "settled";
                            // far below dark-scene (<40) vs lit-scene (100+) gaps
    int maxAttempts = 60;   // grab attempts (incl. duplicate/empty frames) before
                            // giving up — bounds the wait on a dead camera to
                            // roughly 600 ms at the pipeline's 10 ms poll pace
};

// Pure decision state over mean-luma samples — no camera, no clock — so the
// semantics stay unit-testable. The caller must feed only DISTINCT frames
// (dedupe via the camera frame sequence); a repeated buffered frame would
// fake a stable window while AGC is still ramping.
class ExposureWarmup {
public:
    explicit ExposureWarmup(ExposureWarmupConfig config = {})
        : m_config(config) {}

    // Feed one distinct frame's mean luma. Returns true when the warmup is
    // complete: a full window within lumaTol after minSamples, or maxSamples
    // reached without ever settling.
    bool Feed(float meanLuma) {
        ++m_samples;
        m_window.push_back(meanLuma);
        if (static_cast<int>(m_window.size()) > m_config.window) {
            m_window.erase(m_window.begin());
        }
        if (m_samples >= m_config.minSamples &&
            static_cast<int>(m_window.size()) == m_config.window) {
            const auto loHi = std::minmax_element(m_window.begin(), m_window.end());
            if (*loHi.second - *loHi.first <= m_config.lumaTol) {
                m_settled = true;
            }
        }
        return m_settled || m_samples >= m_config.maxSamples;
    }

    int Samples() const { return m_samples; }
    bool Settled() const { return m_settled; }

private:
    ExposureWarmupConfig m_config;
    std::vector<float> m_window;
    int m_samples = 0;
    bool m_settled = false;
};

// Subsampled Rec.601 mean luma over the frame — every 4th pixel in each
// direction. That is ~1/16 of the pixels, still resolves AGC convergence to
// a couple of luma units, and keeps the warmup negligible next to inference.
inline float MeanLuma(const FrameImage& image) {
    double sum = 0.0;
    long count = 0;
    for (long r = 0; r < image.nr(); r += 4) {
        for (long c = 0; c < image.nc(); c += 4) {
            const RgbPixel& px = image(r, c);
            sum += 0.299 * px.red + 0.587 * px.green + 0.114 * px.blue;
            ++count;
        }
    }
    return count != 0 ? static_cast<float>(sum / count) : 0.0f;
}

} // namespace facelogin
