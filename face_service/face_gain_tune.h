#pragma once

#include "../common/frame_image.h"
#include "../common/logger.h"
#include "onnx_models.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <functional>
#include <string>
#include <thread>

namespace facelogin {

// Face-priority exposure tune — the fix for the backlit-small-face failure
// mode: the driver's auto-exposure balances the FULL-FRAME average, so a
// small face against a bright background ends up underexposed (real-machine
// dark-domain identity distance +0.25..0.3; BLC and the recognizer-side
// low-light stretch both measured ineffective on 2026-09-09).
//
// Keep a usable face at the shortest practical exposure: pin AE before
// measuring gain response, try gain before lengthening exposure, and reclaim
// a long exposure when one shorter step still has a conservative luma margin.
// Gain response is measured, never assumed; dead/absent gain falls back to
// photons. Detection and brightness are capture-quality evidence only:
// identity and PAD still decide whether any frame can authenticate.
//
// Two-sided: manual settings persist across graph builds (observed), so a
// long exposure tuned in a dark room would white-out a bright scene with no
// way back — when the face measures over-bright the exposure is stepped back
// down toward the band. Tuning failure does not bypass identity or PAD;
// unsupported controls, cancelled waits, lost faces and exhausted budgets
// leave capture at its current settings.
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
    float targetLumaLow = 50.0f;  // exposure stop; correcting gain adds settleTol margin
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
    // Prefer response + plateau + low trend; unchanged pre-write frames
    // must wait settleMs. Both paths require a stable window (see gate).
    int settleFrames = 40;        // per-step sample cap (≈1.3 s at 30 fps)
    float settleTol = 6.0f;       // consecutive readings within this = converged
    int settleMs = 900;           // fallback delay when no directional response is proven
    // Early completion needs an observed directional response followed by
    // a tighter 200 ms plateau. Unchanged pre-write frames cannot qualify.
    int responseStableMs = 200;
    float maxSettleDriftPerSecond = 2.0f; // reject a quiet-looking but continuing ramp
    float maxGainBoostFraction = 0.67f;    // of the span above the start value
    float maxExposureBoostFraction = 0.5f; // lower: long exposures throttle fps
    // Stop proactive shortening at 1/32 s, enough for the requested 30 fps.
    // Shorter exposures may still be needed to correct an over-bright face.
    // This is a preference, not a hard cap: low light can retain longer times.
    long preferredExposureValue = -5;
    float deadKnobRise = 3.0f;    // a knob moving luma less than this per step
                                  // is fake/unwired — abandoned after 2 dead steps
    int maxGrabAttempts = 40;     // per distinct frame, bounds a dead camera
    // Bright-direction ceiling for the exposure knob, in driver units. This
    // camera's UVC exposure is log2 seconds, so the value maps to frame time:
    // 2^-4 s = 62.5 ms ≈ the auth pacing guard (60 ms) — at -4 the camera
    // still delivers ~16 fps and the guard stays the binding interval
    // (measured pacing 61.5 ms), while -3 (125 ms) halves delivery to ~8 fps:
    // every counted frame stretched to 95–118 ms and the exposure warmup
    // stopped converging for six consecutive ~1.0 s rounds (2026-09-13
    // evening, right after a -3 landing). So the up-climb stops here and the
    // remaining lift goes to gain, which costs no frame time. Only the up
    // direction is capped — over-bright recovery (down) is always allowed,
    // and the extreme-dark fallback in TuneFaceExposure may exceed the cap
    // when gain is exhausted (slow-but-passing beats underexposed). A cap
    // below the driver's range min is unenforceable and ignored (a driver
    // with absolute-µs units degrades to the uncapped legacy behavior).
    long exposureUpCapValue = -4;
    // When non-empty, in-band observations AND settled tuning progress persist
    // the current {exposure, gain} combo here. Hosts replay it at camera
    // init (LoadTuneKnobState + ApplyTuneKnobState). The driver intermittently
    // loses manual UVC controls across its idle power-down (2026-09-13: a
    // written -3 read back as the -5 default, an older gain 22 read back as
    // 0), so "remembering" must live app-side. Progress need not reach the
    // target band: otherwise a budget-limited 50->58 gain adjustment is
    // undone by replaying the old 50 every round (2026-09-15).
    std::wstring persistPath;
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
    std::function<bool(bool& manual)> exposureIsManual;
};

// ---------------------------------------------------------------------------
// Persisted last settled sensor combo (not necessarily in the target band)
// ---------------------------------------------------------------------------

struct TuneKnobState {
    bool hasExposure = false;
    long exposure = 0;
    bool hasGain = false;
    long gain = 0;
};

// Two ASCII lines, "exposure=<v>" / "gain=<v>"; a missing line means that
// knob was unsupported at save time. Torn or corrupt content fails the parse
// and is ignored — persistence is best-effort, the tune stays the authority.
inline bool LoadTuneKnobState(const std::wstring& path, TuneKnobState& out) {
    out = TuneKnobState{};
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) return false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long v = 0;
        if (sscanf_s(line, " exposure = %ld", &v) == 1) {
            out.exposure = v;
            out.hasExposure = true;
        } else if (sscanf_s(line, " gain = %ld", &v) == 1) {
            out.gain = v;
            out.hasGain = true;
        }
    }
    fclose(f);
    return out.hasExposure || out.hasGain;
}

inline bool SaveTuneKnobState(const std::wstring& path, const SensorKnobs& knobs) {
    long exposure = 0, gain = 0;
    const bool haveExposure =
        static_cast<bool>(knobs.exposureGet) && knobs.exposureGet(exposure);
    const bool haveGain = static_cast<bool>(knobs.gainGet) && knobs.gainGet(gain);
    if (!haveExposure && !haveGain) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wt") != 0 || !f) return false;
    if (haveExposure) fprintf(f, "exposure=%ld\n", exposure);
    if (haveGain) fprintf(f, "gain=%ld\n", gain);
    fclose(f);
    return true;
}

// Replay a persisted combo at camera init. Values are clamped into the
// driver's current range and equal values are never re-written, so an intact
// driver pays nothing. The exposure write settles during the host's warmup /
// model-load window; the tune that follows remains the authority. Returns
// whether anything was written — the host must treat sensor readings as
// untrustworthy until the write-lag window has passed (the auth worker
// re-measures its trigger after it; deciding a tune on the pre-settle
// reading stacked a second write on top and mis-flagged gain as a dead
// knob, 2026-09-13 21:59: one 4.3 s round landing over-bright).
inline bool ApplyTuneKnobState(const SensorKnobs& knobs, const TuneKnobState& st) {
    bool wrote = false;
    long mn = 0, mx = 0, stp = 0, cur = 0;
    if (st.hasExposure &&
        knobs.exposureRange && knobs.exposureGet && knobs.exposureSet &&
        knobs.exposureRange(mn, mx, stp) && knobs.exposureGet(cur)) {
        const long target = std::clamp(st.exposure, mn, mx);
        if (target != cur && knobs.exposureSet(target)) {
            FACELOGIN_INFO(L"Tune state replay: exposure %ld→%ld", cur, target);
            wrote = true;
        }
    }
    if (st.hasGain &&
        knobs.gainRange && knobs.gainGet && knobs.gainSet &&
        knobs.gainRange(mn, mx, stp) && knobs.gainGet(cur)) {
        const long target = std::clamp(st.gain, mn, mx);
        if (target != cur && knobs.gainSet(target)) {
            FACELOGIN_INFO(L"Tune state replay: gain %ld→%ld", cur, target);
            wrote = true;
        }
    }
    return wrote;
}

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
    bool fineDown = false;  // exposure: both directions take single granules
    long upCap = LONG_MAX;  // bright-direction ceiling in driver units;
                            // LONG_MAX = uncapped (gain). Exposure passes the
                            // config cap — long exposures throttle fps (see
                            // GainTuneConfig::exposureUpCapValue).
    bool adaptiveGain = false; // gain only: use settled step response to approach
                               // an interior target without repeated tiny steps
    const wchar_t* name = L"knob"; // diagnostics only
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

// Pure wall-time gate, also replayed against real-camera response CSVs.
// 900 ms remains the no-response fallback; it is not a mandatory delay when
// the actuator has visibly responded and its new output has stopped moving.
class FaceSettleGate {
public:
    FaceSettleGate(const GainTuneConfig& cfg, float before, int direction)
        : cfg_(cfg), before_(before), direction_(direction) {}
    bool Feed(float luma, long long elapsedMs) {
        if (!std::isfinite(luma) || luma < 0 || elapsedMs < lastMs_) return false;
        const bool fallback = prev_ >= 0 && elapsedMs >= cfg_.settleMs &&
                              std::fabs(luma - prev_) <= cfg_.settleTol;
        prev_ = luma;
        lastMs_ = elapsedMs;
        // Zero delay is reserved for deterministic controller tests.
        if (cfg_.settleMs == 0) {
            if (fallback) confirmedEarly_ = false;
            return fallback;
        }
        const float responseFloor = std::max(cfg_.settleTol, std::fabs(before_) * 0.05f);
        plateau_.push_back({elapsedMs, luma});
        // Keep a contiguous window whose total range fits the tighter band.
        while (!plateau_.empty()) {
            float lo = luma, hi = luma;
            for (const auto& item : plateau_) {
                lo = std::min(lo, item.second);
                hi = std::max(hi, item.second);
            }
            if (hi - lo <= cfg_.settleTol * 0.5f) break;
            plateau_.pop_front();
        }
        if (plateau_.size() < 4 ||
            elapsedMs - plateau_.front().first < cfg_.responseStableMs) return false;
        bool responseObserved = direction_ != 0 && before_ >= 0;
        for (const auto& item : plateau_)
            if ((item.second - before_) * direction_ < responseFloor) responseObserved = false;
        if (!fallback && !responseObserved) return false;
        // A small range alone accepts the early flat-looking part of a slow
        // ramp. Regress brightness against time to reject residual drift.
        double sumT = 0, sumY = 0, sumTT = 0, sumTY = 0;
        for (const auto& [ms, value] : plateau_) {
            const double t = (ms - plateau_.front().first) / 1000.0;
            sumT += t; sumY += value; sumTT += t*t; sumTY += t*value;
        }
        const double n = static_cast<double>(plateau_.size());
        const double denom = n*sumTT - sumT*sumT;
        const bool confirmed = denom > 0 && std::fabs((n*sumTY - sumT*sumY) / denom) <=
                               cfg_.maxSettleDriftPerSecond;
        if (confirmed) confirmedEarly_ = !fallback;
        return confirmed;
    }
    // Valid after Feed returned true: confirmed by observed directional
    // response + plateau before the settleMs floor, vs the no-response floor.
    bool ConfirmedEarly() const { return confirmedEarly_; }
private:
    const GainTuneConfig& cfg_;
    float before_;
    int direction_;
    float prev_ = -1.0f;
    long long lastMs_ = -1;
    bool confirmedEarly_ = false;
    std::deque<std::pair<long long, float>> plateau_;
};

// Shared gate for actuator writes. A face lost during settling, cancellation
// or the sample cap never authorizes another write or persistence. Every exit
// logs one line: how long the knob wait took and, on failure, why the
// measurement was not confirmed (post-mortem 2026-09-19 17:46:32: a 151 ms
// "unconfirmed" with all-knob "n/a" left the reason guessable).
inline bool SettleFace(const std::function<bool(float&)>& measure,
                       const std::function<bool()>& isCancelled,
                       const GainTuneConfig& cfg, float& luma,
                       float before = -1.0f, int direction = 0) {
    const auto start = std::chrono::steady_clock::now();
    FaceSettleGate gate(cfg, before, direction);
    for (int f = 0; f < cfg.settleFrames; ++f) {
        float sample = -1.0f;
        if (isCancelled() || !measure(sample) || isCancelled() ||
            !std::isfinite(sample) || sample < 0.0f) {
            FACELOGIN_INFO(L"Sensor settle unconfirmed after %lld ms: %s",
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start).count(),
                           isCancelled() ? L"cancelled" : L"face lost or grab failed");
            return false;
        }
        if (gate.Feed(sample, std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - start).count())) {
            luma = sample;
            FACELOGIN_INFO(L"Sensor settle: face luma %.0f after %lld ms (%s)",
                           sample,
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start).count(),
                           gate.ConfirmedEarly()
                               ? L"early — response + plateau"
                               : L"no-response floor");
            return true;
        }
    }
    FACELOGIN_INFO(L"Sensor settle unconfirmed after %lld ms: sample budget (%d frames) "
                   L"without a stable window",
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count(),
                   cfg.settleFrames);
    return false;
}

// Only shorten if the next driver granule is predicted to retain the full
// brightness floor plus hysteresis margin. Never speculate by doubling gain
// and halving exposure together: that assumes an unmeasured gain transfer.
inline bool CanShortenExposure(float luma, const SensorKnobs& knobs,
                                const GainTuneConfig& cfg) {
    long mn = 0, mx = 0, step = 0, cur = 0;
    if (!knobs.exposureRange || !knobs.exposureGet || !knobs.exposureSet ||
        !knobs.exposureRange(mn, mx, step) || !knobs.exposureGet(cur) ||
        mn >= 0 || mn > mx || cur < mn || cur > mx || step <= 0 || step > 16 ||
        cur <= cfg.preferredExposureValue ||
        static_cast<long long>(cur) - step < std::max(mn, cfg.preferredExposureValue)) return false;
    return luma > (cfg.minFaceLuma + cfg.settleTol) * std::exp2(static_cast<float>(step));
}

// Climb one knob toward the band ("up") or down from over-bright. Returns
// steps applied and the final value via out params. A knob that moves luma
// less than deadKnobRise for two consecutive steps is declared dead — the
// driver accepts writes but the sensor ignores them (seen 2026-09-10).
inline int StepKnob(const KnobTrio& knob,
                    bool up,
                    float& luma,
                    const std::function<bool(float&)>& measure,
                    const std::function<bool()>& isCancelled,
                    const GainTuneConfig& cfg,
                    wchar_t* deltaText, size_t deltaTextLen,
                    bool& measurementValid,
                    int* deadStepsOut = nullptr) {
    long mn = 0, mx = 0, st = 0;
    long cur = 0;
    if (!knob.range || !knob.get || !knob.set || !knob.range(mn, mx, st)) return 0;
    if (!knob.get(cur) || mn > mx || cur < mn || cur > mx) return 0;
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
    long limit = up ? cur + boost : cur - boost;
    // Bright-direction cap (exposure only): stop the climb at the cap even
    // when the boost budget would go further; gain covers the remainder. A
    // cap below the driver's range min is unenforceable (every legal value
    // is brighter than the cap) and ignored there; a cap above the max is a
    // natural no-op. If the knob already sits brighter than the cap (a
    // legacy value from before the cap existed), delta goes <= 0 and the
    // loop leaves it — only an over-bright event walks it back down.
    if (up && knob.upCap != LONG_MAX && knob.upCap >= mn) {
        limit = std::min(limit, knob.upCap);
    }
    int steps = 0;
    int deadSteps = 0;
    const long startVal = cur;
    // Starting a correction at <50 and stopping exactly at 50 re-triggers on
    // the next graph's small measurement variation. Gain can land inside the
    // band without paying exposure's frame-rate cost; reserve one measurement
    // tolerance. Exposure retains its original stop line and upward cap.
    const float stopLuma = up && knob.adaptiveGain
        ? std::min(cfg.targetLumaHigh, cfg.targetLumaLow + cfg.settleTol)
        : cfg.targetLumaLow;
    float gainResponse = 0.0f;
    long measuredMove = 0;
    // Each step moves half the remaining span toward the limit, but never
    // less than one driver granule, and lands exactly on the limit — plain
    // integer halving ((limit-cur)/2 == 0 → break) stopped dead once the
    // remaining span dropped below 2.
    const long granule = st > 0 ? st : 1;
    while (steps < cfg.maxSteps &&
           (up ? luma < stopLuma : luma > cfg.maxFaceLuma)) {
        if (isCancelled()) { measurementValid = false; break; }
        // After a responsive, settled gain step, the actual driver range is
        // the useful bound. The old fraction-of-headroom budget stranded a
        // working knob below target for several separate unlocks.
        const bool predictGain = up && knob.adaptiveGain && gainResponse > 0.0f;
        const long delta = up ? (predictGain ? mx : limit) - cur : cur - limit;
        if (delta <= 0) break;
        // Gain uses half-span or measured-response prediction. Exposure
        // takes one granule in either direction to avoid unnecessary long
        // exposure and over-bright/dark oscillation between graph inits.
        long move;
        if (predictGain) {
            // Secant estimate from this round's measured response. Cap the
            // extrapolation at twice the tested step and the physical range;
            // every prediction must still pass the full settle/measure gate.
            const double requested = std::ceil(
                static_cast<double>(stopLuma - luma) / gainResponse / granule) * granule;
            const double bound = std::min(static_cast<double>(delta),
                                           2.0 * measuredMove);
            move = static_cast<long>(std::clamp(requested, 1.0, bound));
        } else if (!knob.fineDown) {
            move = (delta + 1) / 2;                        // ceil of half-span
            move = ((move + granule - 1) / granule) * granule;
            move = std::min(std::max(move, 1L), delta);    // clamp to limit
        } else {
            move = std::min(granule, delta);
        }
        const long next = up ? cur + move : cur - move;
        if ((up && next <= cur) || (!up && next >= cur)) break;
        if (!knob.set(next)) break;
        const long prevVal = cur;
        cur = next;
        ++steps;
        measurementValid = false;
        float measured = -1.0f;
        const auto stepStart = std::chrono::steady_clock::now();
        measurementValid = SettleFace(measure, isCancelled, cfg, measured, luma, up ? 1 : -1);
        FACELOGIN_INFO(L"Tune step: %s %ld→%ld, face luma %.0f→%.0f — %s (%lld ms)",
                       knob.name, prevVal, cur, luma, measured,
                       measurementValid ? L"settled" : L"unconfirmed",
                       static_cast<long long>(
                           std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - stepStart).count()));
        // A sample-budget exit is not proof that the write has settled.
        // Do not persist it or stack another knob write on stale readings.
        if (!measurementValid) break;
        if (up && knob.adaptiveGain) {
            const float rise = measured - luma;
            gainResponse = rise >= cfg.deadKnobRise
                ? rise / static_cast<float>(move) : 0.0f;
            measuredMove = move;
        }
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
    if (deadStepsOut) *deadStepsOut = deadSteps;
    return steps;
}

} // namespace detail

// Core entry: firstLuma is an already-measured face luma. In-band scenes
// are a no-op unless there is enough light to reclaim a slow exposure.
// Returns the number of steps applied on any knob. Always logs one summary
// line when action was attempted.
inline int TuneFaceExposure(float firstLuma,
                            const SensorKnobs& knobs,
                            const std::function<bool(float&)>& measure,
                            const std::function<bool()>& isCancelled,
                            const GainTuneConfig& cfg = {}) {
    if (!std::isfinite(firstLuma) || firstLuma < 0.0f || isCancelled()) return 0;
    const bool shorten = detail::CanShortenExposure(firstLuma, knobs, cfg);
    if (firstLuma >= cfg.minFaceLuma && firstLuma <= cfg.maxFaceLuma && !shorten) {
        // In-band: whatever the knobs hold right now is a certified combo —
        // refresh the persisted state so the next camera init can replay it
        // even after the driver forgets its manual controls.
        if (!cfg.persistPath.empty()) SaveTuneKnobState(cfg.persistPath, knobs);
        return 0;
    }
    const bool up = firstLuma < cfg.minFaceLuma;
    const auto tuneStart = std::chrono::steady_clock::now();

    bool measurementValid = true;
    float luma = firstLuma;
    wchar_t expText[96] = L"n/a";
    wchar_t gainText[96] = L"n/a";
    int steps = 0;
    int gainDeadSteps = 0;
    bool gainTextWritten = false;
    // Write position for the next gain delta segment: the first pass
    // overwrites the "n/a" placeholder, later passes append after "; ".
    const auto gainSeg = [&]() -> wchar_t* {
        if (!gainTextWritten) {
            gainTextWritten = true;
            return gainText;
        }
        const size_t len = wcslen(gainText);
        if (len + 2 < 96) wcscpy_s(gainText + len, 96 - len, L"; ");
        return gainText + wcslen(gainText);
    };

    // AE can silently cancel gain. Freeze its current exposure (without
    // lengthening it), then measure a settled face before trying gain.
    bool manual = false;
    const bool knownManual = knobs.exposureIsManual && knobs.exposureIsManual(manual) && manual;
    bool gainFirst = up && knownManual;
    if (up && !knownManual && knobs.exposureGet && knobs.exposureSet) {
        long current = 0;
        if (knobs.exposureGet(current) && !isCancelled() && knobs.exposureSet(current)) {
            ++steps;
            const float beforeTakeover = luma;
            const auto takeoverStart = std::chrono::steady_clock::now();
            measurementValid = detail::SettleFace(measure, isCancelled, cfg, luma);
            FACELOGIN_INFO(L"AE takeover: exposure pinned at %ld, face luma %.0f→%.0f — "
                           L"%s (%lld ms)",
                           current, beforeTakeover, luma,
                           measurementValid ? L"settled" : L"unconfirmed",
                           static_cast<long long>(
                               std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - takeoverStart).count()));
            gainFirst = measurementValid;
        } else {
            measurementValid = false;
            FACELOGIN_INFO(L"AE takeover unavailable — gain tuning skipped this round");
        }
    }
    int gainSteps = 0;
    if (measurementValid && gainFirst && detail::KnobReady(knobs.gainRange, knobs.gainSet)) {
        const detail::KnobTrio gain{
            knobs.gainRange, knobs.gainGet, knobs.gainSet,
            cfg.maxGainBoostFraction, false, LONG_MAX, true, L"gain"};
        wchar_t* seg = gainSeg();
        gainSteps = detail::StepKnob(gain, true, luma, measure, isCancelled,
                                    cfg, seg, 96 - static_cast<size_t>(seg - gainText),
                                    measurementValid, &gainDeadSteps);
        steps += gainSteps;
    }
    long gainMin = 0, gainMax = 0, gainStep = 0, gainCurrent = 0;
    const bool gainHasHeadroom = knobs.gainRange && knobs.gainGet && knobs.gainSet &&
        knobs.gainRange(gainMin, gainMax, gainStep) && knobs.gainGet(gainCurrent) &&
        gainCurrent < gainMax && gainDeadSteps < 2;
    const bool gainBudgetPending = gainFirst && gainSteps >= cfg.maxSteps && gainHasHeadroom;
    // Exposure uses SINGLE granules in both directions. A jump of several
    // stops overshoots and commits a permanent frame-rate penalty. Gain has
    // already had its chance to retain the shorter capture time.
    if (measurementValid && !gainBudgetPending &&
        (luma < cfg.minFaceLuma || luma > cfg.maxFaceLuma || shorten) &&
        detail::KnobReady(knobs.exposureRange, knobs.exposureSet)) {
        const detail::KnobTrio exposure{
            knobs.exposureRange, knobs.exposureGet, knobs.exposureSet,
            cfg.maxExposureBoostFraction, true, cfg.exposureUpCapValue,
            false, L"exposure"};
        steps += detail::StepKnob(exposure, luma < cfg.minFaceLuma, luma, measure, isCancelled,
                                  cfg, expText, 96, measurementValid);
    }
    // An in-band face can still be using an unnecessarily slow exposure.
    // Only reclaim a step with a predicted post-step luma > floor + margin.
    // Verify each step, stop immediately if its response differs, and let
    // the normal gain/fallback stages repair any measured underexposure.
    for (int attempt = 0; measurementValid && attempt < cfg.maxSteps &&
         detail::CanShortenExposure(luma, knobs, cfg); ++attempt) {
        long cur = 0, mn = 0, mx = 0, granule = 0;
        if (isCancelled()) { measurementValid = false; break; }
        if (!knobs.exposureGet(cur) || !knobs.exposureRange(mn, mx, granule) ||
            !knobs.exposureSet(cur - granule)) break;
        ++steps;
        const float before = luma;
        measurementValid = detail::SettleFace(measure, isCancelled, cfg, luma, before, -1);
        FACELOGIN_INFO(L"Short exposure preference: %ld→%ld, face %.0f→%.0f (%s)",
                       cur, cur - granule, before, luma,
                       measurementValid ? L"settled" : L"unconfirmed");
        if (!measurementValid || before - luma < cfg.deadKnobRise) break;
    }
    // Gain is the fine actuator — and the designated lifter once exposure
    // caps out: amplification adds no frame time. Exposure is quantized in
    // ~2× steps — wider than the nominal 50–90 band — so in some ambient
    // light NO exposure state lands in band (measured 2026-09-13:
    // luma(-3)≈45–54 vs luma(-2)≈137–165 — hunting between them is
    // unavoidable on exposure alone). Whatever way the tune came in, trim
    // gain toward the nominal band on the landed state; gain also covers
    // what exposure cannot: persisted dark-scene gain keeps a brighter room
    // washed even at the exposure floor (2026-09-13: exposure -2 + gain 54
    // read face luma 178).
    const bool gainUp = luma < cfg.targetLumaLow;
    const bool gainDown = luma > cfg.targetLumaHigh;
    if (measurementValid && (gainUp || gainDown) && gainDeadSteps < 2 &&
        (!gainUp || gainSteps < cfg.maxSteps) &&
        detail::KnobReady(knobs.gainRange, knobs.gainSet)) {
        const detail::KnobTrio gain{
            knobs.gainRange, knobs.gainGet, knobs.gainSet,
            cfg.maxGainBoostFraction, false, LONG_MAX, true, L"gain"};
        wchar_t* seg = gainSeg();
        GainTuneConfig remaining = cfg;
        if (gainUp) remaining.maxSteps -= gainSteps;
        steps += detail::StepKnob(gain, gainUp, luma, measure,
                                  isCancelled, remaining, seg,
                                  96 - static_cast<size_t>(seg - gainText),
                                  measurementValid, &gainDeadSteps);
    }
    // Extreme-dark fallback: the cap plus a maxed (or dead/absent) gain
    // still leave the face under minFaceLuma — the room is darker than
    // in-band can reach. Spend frame rate for photons: one uncapped
    // exposure pass. ~8 fps beats an underexposed face that misses the
    // identity threshold entirely.
    bool gainExhausted = !knobs.gainGet || !detail::KnobReady(knobs.gainRange, knobs.gainSet);
    if (!gainExhausted) {
        long gmn = 0, gmx = 0, gstp = 0, gcur = 0;
        gainExhausted = gainDeadSteps >= 2 ||
            (knobs.gainRange(gmn, gmx, gstp) && knobs.gainGet(gcur) &&
             gcur >= gmx);
    }
    if (measurementValid && luma < cfg.minFaceLuma && gainExhausted &&
        detail::KnobReady(knobs.exposureRange, knobs.exposureSet)) {
        if (wcscmp(expText, L"n/a") == 0) expText[0] = L'\0';
        const size_t expLen = wcslen(expText);
        if (expLen > 0 && expLen + 2 < 96) wcscpy_s(expText + expLen, 96 - expLen, L"; ");
        const detail::KnobTrio uncappedExposure{
            knobs.exposureRange, knobs.exposureGet, knobs.exposureSet,
            cfg.maxExposureBoostFraction, true, LONG_MAX, false, L"exposure"};
        // Direction is always "brighten": the trigger is luma below the
        // band floor, so an over-bright entry that overshot past it must
        // climb back — `up` would darken here.
        steps += detail::StepKnob(uncappedExposure, true, luma, measure,
                                  isCancelled, cfg,
                                  expText + wcslen(expText),
                                  96 - wcslen(expText), measurementValid);
    }

    FACELOGIN_INFO(L"Face exposure tune: face luma %.0f (%s) → exposure %s, gain %s → "
                   L"face luma now %.0f (target %.0f–%.0f, %s) — %lld ms",
                   firstLuma, up ? L"dark" : (shorten ? L"shorten" : L"over-bright"), expText, gainText,
                    luma, cfg.minFaceLuma, cfg.maxFaceLuma,
                    measurementValid ? L"settled" : L"unconfirmed",
                   static_cast<long long>(
                       std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - tuneStart).count()));
    if (!cfg.persistPath.empty() && steps > 0 && measurementValid && !isCancelled()) {
        // Save progress even when the per-round budget ends outside the band.
        // The next round resumes here, and can eventually exhaust gain and
        // reach the existing exposure fallback in a genuinely dark scene.
        if (!SaveTuneKnobState(cfg.persistPath, knobs)) {
            FACELOGIN_WARN(L"Failed to save settled camera tune progress");
        }
    }
    return steps;
}

// Production measurement adapter. Tests exercise the same controller with
// deterministic sensor feedback, without a camera or ONNX model.
inline int TuneFaceExposure(float firstLuma,
                            const SensorKnobs& knobs,
                            const std::function<bool(FrameImage&, unsigned long long&)>& grab,
                            OnnxDetector& detector,
                            const std::function<bool()>& isCancelled,
                            int cameraRotation,
                            const GainTuneConfig& cfg = {}) {
    unsigned long long lastSeq = 0;
    bool haveSeq = false;
    return TuneFaceExposure(firstLuma, knobs, [&](float& luma) {
        return detail::GrabMeasure(grab, detector, isCancelled, cameraRotation,
                                   cfg, lastSeq, haveSeq, luma);
    }, isCancelled, cfg);
}

// Conservative white-out evidence: widespread clipping, including every
// central tile. A bright window around a dark central face must not trigger
// full-frame metering. This is a recovery heuristic, not an identity signal.
inline bool IsSceneWhiteout(const FrameImage& frame) {
    if (frame.nr() < 5 || frame.nc() < 5) return false;
    int clippedTiles = 0;
    for (int ty = 0; ty < 5; ++ty) {
        for (int tx = 0; tx < 5; ++tx) {
            int count = 0, clipped = 0;
            for (long y = ty * frame.nr() / 5; y < (ty + 1) * frame.nr() / 5; y += 4) {
                for (long x = tx * frame.nc() / 5; x < (tx + 1) * frame.nc() / 5; x += 4) {
                    const auto& p = frame(y, x);
                    ++count;
                    if (0.299 * p.red + 0.587 * p.green + 0.114 * p.blue >= 245.0) ++clipped;
                }
            }
            const bool saturated = count > 0 && clipped * 10 >= count * 7;
            if (tx >= 1 && tx <= 3 && ty >= 1 && ty <= 3 && !saturated) return false;
            if (saturated) ++clippedTiles;
        }
    }
    return clippedTiles >= 20;
}

struct WhiteoutSample {
    bool whiteout = false;
    float faceLuma = -1.0f;
};

// Bounded no-face escape path. Each measurement must be a fresh frame.
// Confirm the initial scene for a full actuator-lag window too: the host
// may just have replayed manual controls. Never persist no-face settings.
// recoveredLuma is valid only after a settled face observation, allowing
// the caller to hand back to face-priority tuning without stacking writes.
inline int RecoverSceneWhiteout(
    const SensorKnobs& knobs,
    const std::function<bool(WhiteoutSample&)>& measure,
    const std::function<bool()>& isCancelled,
    float& recoveredLuma,
    const GainTuneConfig& cfg = {}) {
    recoveredLuma = -1.0f;
    if (!knobs.exposureRange || !knobs.exposureGet || !knobs.exposureSet) return 0;
    int steps = 0;
    while (true) {
        const auto start = std::chrono::steady_clock::now();
        WhiteoutSample prev;
        bool havePrev = false, settled = false;
        WhiteoutSample sample;
        for (int f = 0; f < cfg.settleFrames; ++f) {
            if (isCancelled() || !measure(sample)) return steps;
            // No evidence at entry: leave ordinary empty/dark scenes alone.
            if (steps == 0 && !sample.whiteout && sample.faceLuma < 0.0f) return 0;
            const bool face = std::isfinite(sample.faceLuma) && sample.faceLuma >= 0.0f;
            const bool prevFace = std::isfinite(prev.faceLuma) && prev.faceLuma >= 0.0f;
            const bool agrees = havePrev && face == prevFace &&
                (face ? std::fabs(sample.faceLuma - prev.faceLuma) <= cfg.settleTol
                      : sample.whiteout == prev.whiteout);
            if (agrees && std::chrono::steady_clock::now() - start >=
                               std::chrono::milliseconds(cfg.settleMs)) {
                settled = true;
                break;
            }
            prev = sample;
            havePrev = true;
        }
        if (!settled || isCancelled()) return steps;
        if (sample.faceLuma >= 0.0f && std::isfinite(sample.faceLuma)) {
            recoveredLuma = sample.faceLuma;
            return steps;
        }
        if (!sample.whiteout || steps >= cfg.maxSteps) return steps;
        long mn = 0, mx = 0, step = 0, cur = 0;
        if (!knobs.exposureRange(mn, mx, step) || !knobs.exposureGet(cur) ||
            mn > mx || cur <= mn || cur > mx || step <= 0) return steps;
        const long next = static_cast<long>(std::max(static_cast<long long>(mn),
            static_cast<long long>(cur) - step));
        if (isCancelled() || !knobs.exposureSet(next)) return steps;
        ++steps;
        FACELOGIN_INFO(L"No-face whiteout recovery: exposure %ld→%ld (%d/%d)",
                       cur, next, steps, cfg.maxSteps);
    }
}

// Self-contained variant for hosts without a warmup-produced detection
// (console preview): measures the trigger on a fresh distinct frame, then
// delegates. No-face white-out gets a bounded recovery; normal empty rooms
// stay silent and retain their controls.
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
        if (isCancelled()) return 0;
        FrameImage frame;
        unsigned long long seq = 0;
        if (grab(frame, seq) && (!haveSeq || seq != lastSeq)) {
            haveSeq = true;
            lastSeq = seq;
            RotateFrame(frame, cameraRotation);
            const auto det = detector.DetectLargestFace(frame);
            if (!det) {
                if (!IsSceneWhiteout(frame)) continue;
                float recoveredLuma = -1.0f;
                const int steps = RecoverSceneWhiteout(knobs, [&](WhiteoutSample& sample) {
                    for (int retry = 0; retry < cfg.maxGrabAttempts; ++retry) {
                        if (isCancelled()) return false;
                        FrameImage fresh;
                        unsigned long long freshSeq = 0;
                        if (grab(fresh, freshSeq) && freshSeq > lastSeq) {
                            lastSeq = freshSeq;
                            RotateFrame(fresh, cameraRotation);
                            const auto face = detector.DetectLargestFace(fresh);
                            sample.faceLuma = face ? FaceBoxLuma(fresh, *face) : -1.0f;
                            sample.whiteout = !face && IsSceneWhiteout(fresh);
                            return true;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    return false;
                }, isCancelled, recoveredLuma, cfg);
                if (recoveredLuma >= 0.0f && !isCancelled()) {
                    return steps + TuneFaceExposure(recoveredLuma, knobs, grab, detector,
                                                    isCancelled, cameraRotation, cfg);
                }
                return steps;
            }
            const float luma = FaceBoxLuma(frame, *det);
            if (luma < 0.0f) return 0;
            if (luma >= cfg.minFaceLuma && luma <= cfg.maxFaceLuma &&
                !detail::CanShortenExposure(luma, knobs, cfg)) {
                FACELOGIN_INFO(L"Face exposure tune: not needed (face luma %.0f)", luma);
                if (!cfg.persistPath.empty()) SaveTuneKnobState(cfg.persistPath, knobs);
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
