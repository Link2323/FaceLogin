#include "auth_pipeline.h"
#include "auth_frame_preparation.h"
#include "auth_pipeline_diagnostics.h"
#include "../vision/exposure_warmup.h"
#include "../vision/face_align.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <thread>
#include <utility>

namespace facelogin {
using auth_diagnostics::ElapsedMs;
using auth_diagnostics::ExposureTimingProfile;

namespace {
// Owns both the asynchronous job and its frame. Destroying the future joins
// before the frame is destroyed, including cancellation and exception exits.
class WarmupDetection {
public:
    explicit WarmupDetection(OnnxDetector& detector) : m_detector(detector) {}
    WarmupDetection(const WarmupDetection&) = delete;
    WarmupDetection& operator=(const WarmupDetection&) = delete;

    bool Pending() const { return m_future.valid(); }

    void Poll(AuthFrame& seed) {
        if (Pending() && m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            Finish(seed);
    }

    void Start(const FrameImage& frame, unsigned long long sequence, int rotation) {
        m_frame = frame;
        RotateFrame(m_frame, rotation);
        m_sequence = sequence;
        m_grabWall = std::chrono::steady_clock::now();
        m_future = std::async(std::launch::async, [this] {
            return m_detector.DetectLargestFace(m_frame);
        });
    }

    void Finish(AuthFrame& seed) {
        if (!Pending()) return;
        auto detection = m_future.get();
        seed = AuthFrame{true, std::move(m_frame), m_sequence,
                         std::move(detection), m_grabWall};
    }

private:
    OnnxDetector& m_detector;
    FrameImage m_frame;
    unsigned long long m_sequence = 0;
    std::chrono::steady_clock::time_point m_grabWall{};
    // Must be last: its destructor waits before any captured state is released.
    std::future<std::optional<OnnxDetector::Detection>> m_future;
};
} // namespace

// The caller owns the deadline and result. All sensor calls stay on its
// camera/COM thread; only detection runs asynchronously. Every exit joins
// the pending detection before its input frame or detector can be released.
bool AuthPipeline::PrepareFrames(PreparedAuthFrames& prepared,
                                 ExposureTimingProfile& exposureTiming,
                                 const SensorKnobs& sensorKnobs,
                                 const std::function<bool()>& interrupted,
                                 const decltype(AuthPipelineCallbacks::grabFrame)& grabFrame) {
    // Pre-run SCRFD during exposure warmup: the warmup loop only consumes
    // mean luma, so an async thread can detect the newest sample for
    // free. When the gate opens, a frame + detection from warmup seeds the
    // prefetch slot, removing the first serial grab+detect from the
    // critical path. The seed is a real, distinct, rotated camera frame
    // and the first counted frame has no pacing anchor yet, so no
    // invariant is touched. Seeds older than 400 ms (slow dark-room
    // convergence) are discarded and the loop grabs fresh exactly like
    // the legacy behavior.
    constexpr double kSeedMaxAgeMs = 400.0;
    constexpr float kSeedFaceLumaDrift = 10.0f;
    auto& seed = prepared.seed;
    WarmupDetection detection(m_detector);

    // Replay the persisted last-good sensor combo BEFORE the warmup:
    // the driver intermittently forgets its manual UVC controls across
    // the idle power-down (2026-09-13), and without the replay every
    // such round starts at the driver default and pays the full
    // settle-loop tune. A replayed gain lands instantly; a replayed
    // exposure settles inside the warmup window. Equal values are never
    // re-written, so an intact driver pays nothing.
    const auto replay = ReplayExposureState(sensorKnobs, m_config.tuneStatePath);
    const bool replayWrote = replay.wrote;
    const auto replayAt = replay.startedAt;
    const bool usingAutoExposure = replay.usingAutoExposure;
    prepared.usingAutoExposure = usingAutoExposure;
    // With usable gain, the AE policy above does not query the current
    // mode. Confirm manual mode on the camera's COM thread before warmup;
    // unknown modes and rounds that wrote controls retain latest detection.
    bool manualExposure = false;
    const bool stableSeedMode = usingAutoExposure ||
        (!replayWrote && sensorKnobs.exposureIsManual &&
         sensorKnobs.exposureIsManual(manualExposure) && manualExposure);
    exposureTiming.preparationMs = ElapsedMs(replayAt);

    // Let auto-exposure settle before the first PAD frame. Adaptive gate
    // (see exposure_warmup.h): sample the mean luma of distinct frames
    // and proceed once a 2-sample window is stable, or at the 10-sample
    // cap. A settled scene opens the gate after two distinct frames —
    // already stricter than the legacy 3-iteration discard, which usually
    // completed before the camera's first frame — while slow-converging
    // scenes (cold start, dark room) wait for real stability instead of
    // proceeding with under-exposed frames. Failed grabs and duplicate
    // buffered frames (same frame sequence) don't count as samples;
    // maxAttempts bounds a dead camera. Mean luma is rotation-invariant,
    // so warmup frames skip RotateFrame. The 10 ms poll pace keeps the
    // alignment waste to a fresh 30 fps frame at ~5 ms average.
    const ExposureWarmupConfig warmupConfig;
    ExposureWarmup warmup(warmupConfig);
    const auto warmupStart = std::chrono::steady_clock::now();
    unsigned long long lastSeq = 0;
    bool haveLastSeq = false;
    for (int attempts = 0; attempts < warmupConfig.maxAttempts; ++attempts) {
        if (interrupted()) {
            return false;
        }
        FrameImage warmFrame;
        unsigned long long seq = 0;
        if (!grabFrame(warmFrame, seq) || (haveLastSeq && seq == lastSeq)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const auto previousWarmSeq = lastSeq;
        const bool hadPreviousWarm = haveLastSeq;
        haveLastSeq = true;
        lastSeq = seq;
        detection.Poll(seed);
        const bool gateOpened = warmup.Feed(MeanLuma(warmFrame));
        // A completed detection from the preceding sample already belongs
        // to this stable two-frame window. Keep it when the exposure mode
        // is known and face luma agrees, rather than start another
        // detection and move the first
        // counted frame's pacing anchor forward by one camera period.
        // An unstable/capped window, control write, missing face or older
        // sample keeps the original latest/pending detection path.
        bool reuseCompleted = gateOpened && warmup.Settled() &&
            stableSeedMode && !replayWrote && hadPreviousWarm &&
            seed.valid && seed.det && seed.seq == previousWarmSeq &&
            ElapsedMs(seed.grabWall) <= kSeedMaxAgeMs;
        if (reuseCompleted) {
            FrameImage latest = warmFrame;
            RotateFrame(latest, m_config.cameraRotation);
            const GainTuneConfig bounds;
            const float olderLuma = FaceBoxLuma(seed.frame, *seed.det);
            const float latestLuma = FaceBoxLuma(latest, *seed.det);
            reuseCompleted = olderLuma >= bounds.minFaceLuma &&
                olderLuma <= bounds.maxFaceLuma &&
                latestLuma >= bounds.minFaceLuma &&
                latestLuma <= bounds.maxFaceLuma &&
                std::fabs(latestLuma - olderLuma) <= kSeedFaceLumaDrift;
        }
        exposureTiming.reusedCompletedSeed = reuseCompleted;
        if (!detection.Pending() && !reuseCompleted)
            detection.Start(warmFrame, seq, m_config.cameraRotation);
        if (gateOpened) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    prepared.gateAt = std::chrono::steady_clock::now();
    exposureTiming.gateReached = true;
    FACELOGIN_INFO(L"Exposure warmup: %d samples, %.0f ms%s",
                   warmup.Samples(),
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - warmupStart).count(),
                   warmup.Settled() ? L" — stable" : L" — cap reached, proceeding");

    // Only the log consumes this snapshot. Read on the camera's current
    // COM thread while the outstanding SCRFD runs on its existing async
    // thread; do not add concurrent sensor calls or reuse it for tuning.
    auth_diagnostics::ExposureSnapshot diagnosticSnapshot;
    const auto readDiagnosticKnobs = [&] {
        diagnosticSnapshot.ReadOnce(sensorKnobs, m_callbacks.readExposureState, exposureTiming);
    };
    // If replay wrote a knob, defer the snapshot until after its existing
    // settle/re-measure path, so the log cannot describe pre-write state.
    if (!replayWrote && detection.Pending()) {
        readDiagnosticKnobs();
        exposureTiming.triggerQueriesBeforeSeedWait = true;
    }

    const auto seedWaitAt = std::chrono::steady_clock::now();
    detection.Finish(seed);
    exposureTiming.seedWaitMs = ElapsedMs(seedWaitAt);
    if (interrupted()) return false;
    exposureTiming.warmupSeedSeq = seed.valid ? seed.seq : 0;
    exposureTiming.warmupSeedAgeMs = seed.valid ? ElapsedMs(seed.grabWall) : -1.0;
    if (seed.valid && std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - seed.grabWall).count()
                          > kSeedMaxAgeMs) {
        seed = AuthFrame{};                      // stale seed: grab fresh like the legacy loop
    }

    // Face-priority gain tune (face_gain_tune.h): the trigger measurement
    // reuses the seed's detection, so bright scenes — the common case —
    // pay one bbox luma pass and zero extra inference. In-band rounds
    // take the early return, which also refreshes the persisted combo;
    // only an actual tune invalidates the seed frame.
    auto& tuneCfg = prepared.tuneConfig;
    tuneCfg.persistPath = m_config.tuneStatePath;
    if (!tuneCfg.persistPath.empty()) tuneCfg.recovery = &prepared.tuneRecovery;
    tuneCfg.writes = &prepared.tuneWrites;
    const auto seedRefreshAt = std::chrono::steady_clock::now();
    if (!seed.valid && !interrupted()) {
        FrameImage fresh;
        unsigned long long freshSeq = 0;
        if (m_callbacks.grabFrame(fresh, freshSeq)) {
            RotateFrame(fresh, m_config.cameraRotation);
            auto det = m_detector.DetectLargestFace(fresh);
            seed = AuthFrame{true, std::move(fresh), freshSeq, std::move(det),
                            std::chrono::steady_clock::now()};
        }
    }
    exposureTiming.seedRefreshMs = ElapsedMs(seedRefreshAt);
    // A just-applied replay write needs the driver's lag window (the
    // same ~0.5–1 s family as the tune settle floor) before readings
    // are trustworthy. Judging the trigger on the pre-settle reading
    // stacks a second write on top of the replay and mis-flags the gain
    // knob as dead (2026-09-13 21:59: one 4.3 s round landing
    // over-bright). Out-of-band readings inside that window get one
    // re-measure after it instead. (Curve sampling during this window
    // showed the post-replug sensor is untrustworthy for several
    // seconds no matter what — delayed jump, not a smooth ramp — so the
    // window stays fixed and the first re-measure decides.)
    exposureTiming.phase = ExposureTimingProfile::ReplaySettle;
    const auto replaySettleAt = std::chrono::steady_clock::now();
    if (replayWrote && seed.valid && seed.det) {
        const float lumaNow = FaceBoxLuma(seed.frame, *seed.det);
        if (lumaNow >= 0.0f &&
            (lumaNow < tuneCfg.minFaceLuma || lumaNow > tuneCfg.maxFaceLuma ||
             detail::CanShortenExposure(lumaNow, sensorKnobs, tuneCfg))) {
            const auto settleUntil =
                replayAt + std::chrono::milliseconds(tuneCfg.settleMs);
            FrameImage fresh;
            unsigned long long freshSeq = 0;
            unsigned long long lastObserved = seed.seq;
            while (std::chrono::steady_clock::now() < settleUntil) {
                if (interrupted()) {
                    return false;
                }
                if (m_callbacks.grabFrame(fresh, freshSeq)) {
                    lastObserved = std::max(lastObserved, freshSeq);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            seed = AuthFrame{};
            for (int attempt = 0; attempt < tuneCfg.maxGrabAttempts; ++attempt) {
                if (interrupted()) {
                    return false;
                }
                if (m_callbacks.grabFrame(fresh, freshSeq) && freshSeq > lastObserved) {
                    RotateFrame(fresh, m_config.cameraRotation);
                    auto det = m_detector.DetectLargestFace(fresh);
                    // A lost face must replace the old detection too, so
                    // white-out recovery can run instead of using stale luma.
                    seed = AuthFrame{true, std::move(fresh), freshSeq, std::move(det),
                                    std::chrono::steady_clock::now()};
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    exposureTiming.replaySettleMs = ElapsedMs(replaySettleAt);
    // Post-mortem diagnostic (2026-09-19 17:45:46): a round that failed
    // with face luma 18 / identity distance 0.989 left NO tune log at all
    // because its trigger frame had no face to measure. One line here
    // states the trigger decision and the knob state so "why was tuning
    // skipped" is always answerable from the log alone.
    exposureTiming.phase = ExposureTimingProfile::Trigger;
    const auto triggerAt = std::chrono::steady_clock::now();
    {
        readDiagnosticKnobs();
        float triggerLuma = -1.0f;
        if (seed.valid && seed.det) triggerLuma = FaceBoxLuma(seed.frame, *seed.det);
        prepared.triggerFaceLuma = triggerLuma;
        FACELOGIN_INFO(L"Exposure tune trigger: %s, face luma %.0f — %s",
                       seed.valid ? (seed.det ? L"face" : L"no face")
                                  : L"no fresh frame",
                       triggerLuma, diagnosticSnapshot.Text().c_str());
    }
    exposureTiming.triggerMs = ElapsedMs(triggerAt);
    exposureTiming.phase = ExposureTimingProfile::Tune;
    const auto tuneAt = std::chrono::steady_clock::now();
    auto& darkRecoveryAttempted = prepared.darkRecoveryAttempted;
    if (seed.valid && seed.det) {
        const float faceLuma = FaceBoxLuma(seed.frame, *seed.det);
        if (faceLuma >= 0.0f) {
            const int tuneSteps = TuneFaceExposure(
                faceLuma, sensorKnobs, m_callbacks.grabFrame,
                m_detector, interrupted,
                m_config.cameraRotation, tuneCfg);
            if (tuneSteps > 0) seed = AuthFrame{};
        }
    } else if (seed.valid &&
               (IsSceneWhiteout(seed.frame) || IsSceneBright(seed.frame) ||
                IsSceneDark(seed.frame, tuneCfg))) {
        // Recovery precedes all PAD/identity frames. Never reuse a seed
        // after this path: controls or the camera stream may have moved.
        darkRecoveryAttempted = IsSceneDark(seed.frame, tuneCfg);
        TuneFaceExposure(sensorKnobs, m_callbacks.grabFrame,
                         m_detector, interrupted,
                         m_config.cameraRotation, tuneCfg);
        seed = AuthFrame{};
    }
    exposureTiming.tuneMs = ElapsedMs(tuneAt);
    if (interrupted()) return false;

    return true;
}

} // namespace facelogin
