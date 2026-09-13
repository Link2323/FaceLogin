#pragma once

#include "../common/frame_image.h"
#include "../common/logger.h"
#include "onnx_models.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>

namespace facelogin {

// Face-priority exposure tune — the fix for the backlit-small-face failure
// mode: the driver's auto-exposure balances the FULL-FRAME average, so a
// small face against a bright background ends up underexposed (real-machine
// dark-domain identity distance +0.25..0.3; BLC and the recognizer-side
// low-light stretch both measured ineffective on 2026-09-09).
//
// Two knobs, tried in this order:
//   1. Exposure time (IAMCameraControl) — the only knob that adds photons.
//      Seizing it also removes the AE axis that silently cancels gain.
//   2. Analog gain (VideoProcAmp_Gain) — amplification only. On the
//      2026-09-10 camera (external vid_32e6) this knob proved DEAD: the
//      driver accepted every set while face luma stayed flat (0→36→45→55
//      gain, luma 17–29 throughout), so a knob that fails to move luma is
//      abandoned early instead of burning the budget.
//
// Two-sided: manual settings persist across graph builds (observed), so a
// long exposure tuned in a dark room would white-out a bright scene with no
// way back — when the face measures over-bright the exposure is stepped back
// down toward the band. Fail-open throughout: any unsupported control,
// cancelled wait, lost face, or exhausted budget leaves the capture exactly
// like an untuned graph.
//
// Runs once per capture-graph init, between exposure warmup and the first PAD
// frame (auth worker) / before preview streaming starts (console). The
// worker's trigger measurement reuses the detection its warmup already
// produced, so in-band scenes — the common case — pay one bbox luma pass and
// zero extra inference.
struct GainTuneConfig {
    float minFaceLuma = 50.0f;    // below: dark — tune up. Equal to targetLumaLow
                                  // so no luma is simultaneously under target yet
                                  // "no tune needed" (the old 40 left a 41–49 dead
                                  // zone: console logged "not needed" at 41–45
                                  // while the target band starts at 50)
    float targetLumaLow = 50.0f;  // up-steps stop once face luma >= this...
    float targetLumaHigh = 90.0f; // ...nominal band top (log label only; the
                                  // step loops land anywhere in [min,max] below)
    float maxFaceLuma = 120.0f;   // landing-band top AND over-bright trigger:
                                  // down-steps stop once luma <= this. One
                                  // exposure granule is about a doubling, wider
                                  // than the 50–90 nominal band — a controller
                                  // that must land <= 90 with a 45/89/150
                                  // quantized knob (this camera's measured
                                  // states) can never stop at -3 and ping-pongs
                                  // -2↔-4 forever. 90–120 is a legal landing.
    int maxSteps = 3;             // hard cap on adjust-settle-measure cycles per knob
    // Settle gate — time floor plus agreement. This driver applies an
    // exposure write with a large lag (reads through a 10-frame window stay
    // flat, then the combined effect lands during the NEXT control write's
    // window, 2026-09-13): any shorter settle reads the PRE-step state, so
    // the controller took one extra step every time, declared the working
    // exposure knob "dead" on the way up and drafted gain into the loop —
    // the preview ping-ponged -2↔-4 with gain ratcheting 39→48→54→58.
    // A reading is trusted only after settleMs of wall time AND two
    // consecutive samples agreeing.
    int settleFrames = 40;        // per-step sample cap (≈1.3 s at 30 fps)
    float settleTol = 6.0f;       // consecutive readings within this = converged
    int settleMs = 900;           // minimum wall time per step before trusting
    float maxGainBoostFraction = 0.67f;    // of the span above the start value
    float maxExposureBoostFraction = 0.5f; // lower: long exposures throttle fps
    float deadKnobRise = 3.0f;    // a knob moving luma less than this per step
                                  // is fake/unwired — abandoned after 2 dead steps
    int maxGrabAttempts = 40;     // per distinct frame, bounds a dead camera
};

// Driver-facing exposure/gain controls, injected by each host (auth worker /
// console) so this header stays DirectShow-free. Larger values = brighter on
// both knobs (UVC exposure is log2 seconds; gain is driver units). An empty
// function means "unsupported" — that knob is skipped.
struct SensorKnobs {
    std::function<bool(long& minOut, long& maxOut, long& stepOut)> exposureRange;
    std::function<bool(long& valueOut)> exposureGet;
    std::function<bool(long value)> exposureSet;  // Manual flag
    std::function<bool(long& minOut, long& maxOut, long& stepOut)> gainRange;
    std::function<bool(long& valueOut)> gainGet;
    std::function<bool(long value)> gainSet;      // Manual flag
};

// Mean luma over a pixel rect — the capture condition that dominates
// identity-match distance. Clipped to the frame.
inline float FaceBoxLuma(const FrameImage& frame, float x1, float y1,
                         float x2, float y2) {
    const long x0 = std::max<long>(0, static_cast<long>(x1));
    const long y0 = std::max<long>(0, static_cast<long>(y1));
    const long x1c = std::min<long>(frame.nc() - 1, static_cast<long>(x2));
    const long y1c = std::min<long>(frame.nr() - 1, static_cast<long>(y2));
    double sum = 0.0;
    long n = 0;
    for (long y = y0; y <= y1c; ++y) {
        for (long x = x0; x <= x1c; ++x) {
            const auto& p = frame(y, x);
            sum += 0.299 * p.red + 0.587 * p.green + 0.114 * p.blue;
            ++n;
        }
    }
    return n > 0 ? static_cast<float>(sum / n) : -1.0f;
}

inline float FaceBoxLuma(const FrameImage& frame,
                         const OnnxDetector::Detection& det) {
    return FaceBoxLuma(frame, det.x1, det.y1, det.x2, det.y2);
}

namespace detail {

struct KnobTrio {
    const std::function<bool(long&, long&, long&)>& range;
    const std::function<bool(long&)>& get;
    const std::function<bool(long)>& set;
    float boostFraction;
    bool fineDown = false;  // down steps take single granules (see StepKnob)
};

// One distinct (per frame-sequence) grab, rotated, detected, measured.
// Returns false on cancel / dead camera / face lost mid-tune.
inline bool GrabMeasure(const std::function<bool(FrameImage&, unsigned long long&)>& grab,
                        OnnxDetector& detector,
                        const std::function<bool()>& isCancelled,
                        int cameraRotation,
                        const GainTuneConfig& cfg,
                        unsigned long long& lastSeq,
                        bool& haveSeq,
                        float& lumaOut) {
    for (int attempt = 0; attempt < cfg.maxGrabAttempts; ++attempt) {
        if (isCancelled()) return false;
        FrameImage frame;
        unsigned long long seq = 0;
        if (grab(frame, seq) && (!haveSeq || seq != lastSeq)) {
            haveSeq = true;
            lastSeq = seq;
            RotateFrame(frame, cameraRotation);
            const auto det = detector.DetectLargestFace(frame);
            if (!det) return false;
            lumaOut = FaceBoxLuma(frame, *det);
            return lumaOut >= 0.0f;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

inline bool KnobReady(const std::function<bool(long&, long&, long&)>& range,
                      const std::function<bool(long)>& set) {
    return static_cast<bool>(range) && static_cast<bool>(set);
}

// Climb one knob toward the band ("up") or down from over-bright. Returns
// steps applied and the final value via out params. A knob that moves luma
// less than deadKnobRise for two consecutive steps is declared dead — the
// driver accepts writes but the sensor ignores them (seen 2026-09-10).
inline int StepKnob(const KnobTrio& knob,
                    bool up,
                    float& luma,
                    const std::function<bool(FrameImage&, unsigned long long&)>& grab,
                    OnnxDetector& detector,
                    const std::function<bool()>& isCancelled,
                    int cameraRotation,
                    const GainTuneConfig& cfg,
                    wchar_t* deltaText, size_t deltaTextLen,
                    unsigned long long& lastSeq,
                    bool& haveSeq) {
    long mn = 0, mx = 0, st = 0;
    long cur = 0;
    if (!knob.range(mn, mx, st)) return 0;
    if (!knob.get(cur)) return 0;
    // Ceil the boost span: floor-halving wedged the exposure knob at one
    // unit of headroom — (mx-cur)*0.5 floors to 0, limit==cur, and no step
    // is ever taken again on any later graph init (real logs: repeated
    // "+0 step(s), -4→-4" on 2026-09-12/13 with range max -1 still unused).
    // Ceil costs at most one extra unit (= one exposure doubling) beyond the
    // boostFraction budget and lets the persistent manual value ratchet to
    // the range top across inits.
    const long span = up ? mx - cur : cur - mn;
    const long boost =
        static_cast<long>(std::ceil(static_cast<float>(span) * knob.boostFraction));
    const long limit = up ? cur + boost : cur - boost;
    int steps = 0;
    int deadSteps = 0;
    const long startVal = cur;
    // Each step moves half the remaining span toward the limit, but never
    // less than one driver granule, and lands exactly on the limit — plain
    // integer halving ((limit-cur)/2 == 0 → break) stopped dead once the
    // remaining span dropped below 2.
    const long granule = st > 0 ? st : 1;
    while (steps < cfg.maxSteps &&
           (up ? luma < cfg.targetLumaLow : luma > cfg.maxFaceLuma)) {
        const long delta = up ? limit - cur : cur - limit;
        if (delta <= 0) break;
        // Up: ceil of half the remaining span — exponential ratchet toward the
        // top, one init at a time. Down on a fineDown knob (exposure): single
        // granules, each settle-measured — exposure units are potent (about a
        // doubling each) and a halved span from a mild over-bright crashes
        // deep into the dark side and flip-flops across inits. Gain units are
        // small and near-linear, so it halves its span both ways.
        long move;
        if (up || !knob.fineDown) {
            move = (delta + 1) / 2;                        // ceil of half-span
            move = ((move + granule - 1) / granule) * granule;
            move = std::min(std::max(move, 1L), delta);    // clamp to limit
        } else {
            move = std::min(granule, delta);
        }
        const long next = up ? cur + move : cur - move;
        if ((up && next <= cur) || (!up && next >= cur)) break;
        if (!knob.set(next)) break;
        cur = next;
        ++steps;
        float measured = -1.0f;
        bool alive = true;
        float prev = -1.0f;
        const auto settleStart = std::chrono::steady_clock::now();
        for (int f = 0; f < cfg.settleFrames && alive; ++f) {
            alive = GrabMeasure(grab, detector, isCancelled, cameraRotation, cfg,
                                lastSeq, haveSeq, measured);
            if (!alive) break;
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - settleStart).count();
            if (prev >= 0.0f && elapsedMs >= cfg.settleMs &&
                std::fabs(measured - prev) <= cfg.settleTol)
                break;  // past the actuator lag and settled on the new value
            prev = measured;
        }
        if (!alive) break;
        if (up ? (measured - luma < cfg.deadKnobRise)
               : (luma - measured < cfg.deadKnobRise)) {
            ++deadSteps;
            if (deadSteps >= 2) { luma = measured; break; }  // fake knob — stop early
        } else {
            deadSteps = 0;
        }
        luma = measured;
    }
    _snwprintf_s(deltaText, deltaTextLen, _TRUNCATE, L"%+d step(s), %ld→%ld%s",
                 steps, startVal, cur,
                 deadSteps >= 2 ? L" (dead knob)" : L"");
    return steps;
}

} // namespace detail

// Core entry: `firstLuma` is an already-measured face luma (either outside
// [minFaceLuma, maxFaceLuma], or inside — in which case this is a no-op).
// Returns the number of steps applied on any knob. Always logs one summary
// line when action was attempted.
inline int TuneFaceExposure(float firstLuma,
                            const SensorKnobs& knobs,
                            const std::function<bool(FrameImage&, unsigned long long&)>& grab,
                            OnnxDetector& detector,
                            const std::function<bool()>& isCancelled,
                            int cameraRotation,
                            const GainTuneConfig& cfg = {}) {
    if (firstLuma < 0.0f ||
        (firstLuma >= cfg.minFaceLuma && firstLuma <= cfg.maxFaceLuma)) {
        return 0;
    }
    const bool up = firstLuma < cfg.minFaceLuma;
    const auto tuneStart = std::chrono::steady_clock::now();

    unsigned long long lastSeq = 0;
    bool haveSeq = false;
    float luma = firstLuma;
    wchar_t expText[64] = L"n/a";
    wchar_t gainText[64] = L"n/a";
    int steps = 0;

    // Exposure first — photons beat amplification, and seizing the exposure
    // axis removes the AE feedback that cancels gain adjustments.
    if (detail::KnobReady(knobs.exposureRange, knobs.exposureSet)) {
        const detail::KnobTrio exposure{
            knobs.exposureRange, knobs.exposureGet, knobs.exposureSet,
            cfg.maxExposureBoostFraction, true};
        steps += detail::StepKnob(exposure, up, luma, grab, detector, isCancelled,
                                  cameraRotation, cfg, expText, 64, lastSeq, haveSeq);
    }
    // Gain is the fine actuator. Exposure is quantized in ~2× steps — wider
    // than the nominal 50–90 band — so in some ambient light NO exposure
    // state lands in band (measured 2026-09-13: luma(-3)≈45–54 vs
    // luma(-2)≈137–165 — hunting between them is unavoidable on exposure
    // alone). Whatever way the tune came in, trim gain toward the nominal
    // band on the landed state; gain also covers what exposure cannot:
    // persisted dark-scene gain keeps a brighter room washed even at the
    // exposure floor (2026-09-13: exposure -2 + gain 54 read face luma 178).
    const bool gainUp = luma < cfg.targetLumaLow;
    const bool gainDown = luma > cfg.targetLumaHigh;
    if ((gainUp || gainDown) &&
        detail::KnobReady(knobs.gainRange, knobs.gainSet)) {
        const detail::KnobTrio gain{
            knobs.gainRange, knobs.gainGet, knobs.gainSet,
            cfg.maxGainBoostFraction};
        steps += detail::StepKnob(gain, gainUp, luma, grab, detector,
                                  isCancelled, cameraRotation, cfg, gainText,
                                  64, lastSeq, haveSeq);
    }

    FACELOGIN_INFO(L"Face exposure tune: face luma %.0f (%s) → exposure %s, gain %s → "
                   L"face luma now %.0f (land %.0f–%.0f) — %lld ms",
                   firstLuma, up ? L"dark" : L"over-bright", expText, gainText,
                   luma, cfg.minFaceLuma, cfg.maxFaceLuma,
                   static_cast<long long>(
                       std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - tuneStart).count()));
    return steps;
}

// Self-contained variant for hosts without a warmup-produced detection
// (console preview): measures the trigger on a fresh distinct frame, then
// delegates. Silent when no face is visible — an empty room is normal.
inline int TuneFaceExposure(const SensorKnobs& knobs,
                            const std::function<bool(FrameImage&, unsigned long long&)>& grab,
                            OnnxDetector& detector,
                            const std::function<bool()>& isCancelled,
                            int cameraRotation,
                            const GainTuneConfig& cfg = {}) {
    if (!knobs.exposureSet && !knobs.gainSet) return 0;
    unsigned long long lastSeq = 0;
    bool haveSeq = false;
    for (int attempt = 0; attempt < cfg.maxGrabAttempts; ++attempt) {
        FrameImage frame;
        unsigned long long seq = 0;
        if (grab(frame, seq) && (!haveSeq || seq != lastSeq)) {
            haveSeq = true;
            lastSeq = seq;
            RotateFrame(frame, cameraRotation);
            const auto det = detector.DetectLargestFace(frame);
            if (!det) continue;  // keep waiting for a face within the budget
            const float luma = FaceBoxLuma(frame, *det);
            if (luma < 0.0f) return 0;
            if (luma >= cfg.minFaceLuma && luma <= cfg.maxFaceLuma) {
                FACELOGIN_INFO(L"Face exposure tune: not needed (face luma %.0f)", luma);
                return 0;
            }
            return TuneFaceExposure(luma, knobs, grab, detector, isCancelled,
                                    cameraRotation, cfg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

} // namespace facelogin
