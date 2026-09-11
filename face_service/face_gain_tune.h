#pragma once

#include "../common/frame_image.h"
#include "../common/logger.h"
#include "onnx_models.h"

#include <algorithm>
#include <chrono>
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
    float minFaceLuma = 40.0f;    // below: dark — tune up
    float targetLumaLow = 50.0f;  // stop once face luma >= this...
    float targetLumaHigh = 90.0f; // ...or overshoots this (still fine, just stop)
    float maxFaceLuma = 180.0f;   // above: over-bright (stuck manual exposure) — tune down
    int maxSteps = 3;             // hard cap on adjust-settle-measure cycles per knob
    int settleFrames = 3;         // distinct frames waited per step
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
    if (!knob.range(mn, mx, st) || !knob.get(cur)) return 0;
    const long limit = up
        ? cur + static_cast<long>(static_cast<float>(mx - cur) * knob.boostFraction)
        : cur - static_cast<long>(static_cast<float>(cur - mn) * knob.boostFraction);
    int steps = 0;
    int deadSteps = 0;
    const long startVal = cur;
    while (steps < cfg.maxSteps &&
           (up ? luma < cfg.targetLumaLow : luma > cfg.targetLumaHigh)) {
        const long half = (limit - cur) / 2;
        if (half == 0) break;
        const long next = cur + (up ? std::max(1L, half) : -std::max(1L, -half));
        if ((up && next <= cur) || (!up && next >= cur)) break;
        if (!knob.set(next)) break;
        cur = next;
        ++steps;
        float measured = -1.0f;
        bool alive = true;
        for (int f = 0; f < cfg.settleFrames && alive; ++f) {
            alive = GrabMeasure(grab, detector, isCancelled, cameraRotation, cfg,
                                lastSeq, haveSeq, measured);
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
            cfg.maxExposureBoostFraction};
        steps += detail::StepKnob(exposure, up, luma, grab, detector, isCancelled,
                                  cameraRotation, cfg, expText, 64, lastSeq, haveSeq);
    }
    // Gain only if exposure left the face dark (never on the down path —
    // analog gain cannot cause white-out).
    if (up && luma < cfg.targetLumaLow &&
        detail::KnobReady(knobs.gainRange, knobs.gainSet)) {
        const detail::KnobTrio gain{
            knobs.gainRange, knobs.gainGet, knobs.gainSet,
            cfg.maxGainBoostFraction};
        steps += detail::StepKnob(gain, true, luma, grab, detector, isCancelled,
                                  cameraRotation, cfg, gainText, 64, lastSeq, haveSeq);
    }

    FACELOGIN_INFO(L"Face exposure tune: face luma %.0f (%s) → exposure %s, gain %s → "
                   L"face luma now %.0f (target %.0f–%.0f) — %lld ms",
                   firstLuma, up ? L"dark" : L"over-bright", expText, gainText,
                   luma, cfg.targetLumaLow, cfg.targetLumaHigh,
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
