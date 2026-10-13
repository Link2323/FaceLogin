#include "auth_pipeline.h"

#include "../common/logger.h"
#include "exposure_warmup.h"
#include "face_align.h"
#include "face_gain_tune.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <thread>
#include <utility>

namespace facelogin {
namespace {

// Main-thread-only, per-request diagnostics. Control reads remain fresh;
// timing does not cache results or change callback arguments/return values.
struct ExposureTimingProfile {
    enum Phase { Replay, ReplaySettle, Trigger, Tune, Loop, PhaseCount };
    enum Operation { ExposureRange, ExposureGet, ExposureSet, GainRange,
                     GainGet, GainSet, ExposureMode, ExposureAuto, ExposureState,
                     OperationCount };
    struct Calls {
        unsigned int count = 0;
        unsigned int failed = 0;
        double totalMs = 0.0;
        double maxMs = 0.0;
    };
    std::array<std::array<Calls, OperationCount>, PhaseCount> calls{};
    Phase phase = Replay;
    bool gateReached = false;
    double seedWaitMs = 0.0;
    double seedRefreshMs = 0.0;
    double replaySettleMs = 0.0;
    double triggerMs = 0.0;
    double triggerQueryMs = 0.0;
    bool triggerQueriesBeforeSeedWait = false;
    bool reusedCompletedSeed = false;
    unsigned long long warmupSeedSeq = 0;
    double warmupSeedAgeMs = -1.0;
    double tuneMs = 0.0;
    double statusMs = 0.0;
    double gateToLoopMs = 0.0;

    ~ExposureTimingProfile() {
        // Emit only after the work, so measuring individual driver calls does
        // not add a log write between them or perturb the warmup schedule.
        if (gateReached) {
            FACELOGIN_INFO(L"Exposure phase timing: seed_wait=%.2f ms, seed_refresh=%.2f ms, "
                           L"replay_settle=%.2f ms, trigger=%.2f ms, tune=%.2f ms, "
                           L"status=%.2f ms, gate_to_loop=%.2f ms, "
                           L"trigger_query=%.2f ms, query_before_seed_wait=%d, "
                           L"seed_reused=%d, seed_seq=%llu, seed_age=%.2f ms",
                           seedWaitMs, seedRefreshMs, replaySettleMs, triggerMs,
                           tuneMs, statusMs, gateToLoopMs, triggerQueryMs,
                           triggerQueriesBeforeSeedWait, reusedCompletedSeed,
                           warmupSeedSeq, warmupSeedAgeMs);
        }
        constexpr const wchar_t* phaseNames[] = {
            L"replay", L"replay_settle", L"trigger", L"tune", L"loop"};
        constexpr const wchar_t* operationNames[] = {
            L"exposure_range", L"exposure_get", L"exposure_set", L"gain_range",
            L"gain_get", L"gain_set", L"exposure_mode", L"exposure_auto",
            L"exposure_state"};
        for (size_t p = 0; p < calls.size(); ++p) {
            for (size_t op = 0; op < calls[p].size(); ++op) {
                const auto& c = calls[p][op];
                if (c.count == 0) continue;
                FACELOGIN_INFO(L"Exposure sensor timing: phase=%s, op=%s, "
                               L"calls=%u, failed=%u, total=%.2f ms, max=%.2f ms",
                               phaseNames[p], operationNames[op], c.count,
                               c.failed, c.totalMs, c.maxMs);
            }
        }
    }
};

double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

SensorKnobs ProfileSensorKnobs(const SensorKnobs& original,
                              ExposureTimingProfile& profile) {
    const auto wrap = [&profile](auto callback, ExposureTimingProfile::Operation op) {
        using Callback = decltype(callback);
        if (!callback) return Callback{};
        return Callback([callback = std::move(callback), &profile, op](auto&&... args) {
            const auto start = std::chrono::steady_clock::now();
            const bool ok = callback(std::forward<decltype(args)>(args)...);
            auto& c = profile.calls[profile.phase][op];
            const double ms = ElapsedMs(start);
            ++c.count;
            if (!ok) ++c.failed;
            c.totalMs += ms;
            c.maxMs = std::max(c.maxMs, ms);
            return ok;
        });
    };
    SensorKnobs result = original;
    result.exposureRange = wrap(original.exposureRange, ExposureTimingProfile::ExposureRange);
    result.exposureGet = wrap(original.exposureGet, ExposureTimingProfile::ExposureGet);
    result.exposureSet = wrap(original.exposureSet, ExposureTimingProfile::ExposureSet);
    result.gainRange = wrap(original.gainRange, ExposureTimingProfile::GainRange);
    result.gainGet = wrap(original.gainGet, ExposureTimingProfile::GainGet);
    result.gainSet = wrap(original.gainSet, ExposureTimingProfile::GainSet);
    result.exposureIsManual = wrap(original.exposureIsManual, ExposureTimingProfile::ExposureMode);
    result.exposureSetAuto = wrap(original.exposureSetAuto, ExposureTimingProfile::ExposureAuto);
    return result;
}

} // namespace

// Mean luma over the face bbox — the capture condition that dominates
// identity-match distance (dark chips shift embeddings; the exposure
// governor, face_gain_tune.h, owns keeping this in band). Diagnostic
// only, computed once per outcome-relevant frame.
static float FaceRegionLuma(const FrameImage& frame, const FaceRect& r) {
    return FaceBoxLuma(frame, static_cast<float>(r.left()),
                       static_cast<float>(r.top()),
                       static_cast<float>(r.right()),
                       static_cast<float>(r.bottom()));
}

AuthPipeline::AuthPipeline(OnnxDetector& detector,
                           OnnxRecognizer& recognizer,
                           OnnxAntiSpoof& antiSpoof,
                           AuthPipelineConfig config,
                           AuthPipelineCallbacks callbacks)
    : m_detector(detector),
      m_recognizer(recognizer),
      m_antiSpoof(antiSpoof),
      m_config(config),
      m_callbacks(std::move(callbacks)) {}

AuthPipelineResult AuthPipeline::Run() {
    AuthPipelineResult result;
    if (!m_callbacks.grabFrame || !m_callbacks.isCancelled ||
        !m_callbacks.isClientDisconnected || !m_callbacks.reportStatus ||
        !m_callbacks.verifyBinding) {
        result.errorMessage = L"认证组件状态异常，请使用密码登录";
        FACELOGIN_ERROR(L"AuthPipeline callbacks are incomplete");
        return result;
    }
    if (!m_antiSpoof.IsInitialized()) {
        result.errorMessage = L"活体检测模块不可用，请使用密码登录";
        FACELOGIN_ERROR(L"AuthPipeline refused to run without anti-spoof");
        return result;
    }

    const auto localDeadline = ipc::MakeAuthDeadline(m_config.authTimeoutSeconds * 1000);
    const auto authDeadline = m_config.authDeadline == ipc::AuthDeadline{} ||
        localDeadline < m_config.authDeadline ? localDeadline : m_config.authDeadline;
    const auto interrupted = [&]() {
        if (m_callbacks.isCancelled()) {
            result.cancelled = true;
            return true;
        }
        if (ipc::RemainingAuthTimeMs(authDeadline) == 0) {
            result.timedOut = true;
            return true;
        }
        return false;
    };
    if (interrupted()) return result;

    try {
        ExposureTimingProfile exposureTiming;
        const SensorKnobs sensorKnobs = ProfileSensorKnobs(m_callbacks.sensorKnobs, exposureTiming);
        // Progressive-learning phase 0 instrumentation: per-round
        // pre-normalization embedding norms (quality signal), one line per
        // round at any exit. RAII so cancel/timeout/attack paths log too.
        struct NormLog {
            std::vector<float> norms;
            unsigned long long attempts = 0, repeatedFrames = 0;
            unsigned long long firstSequence = 0, lastSequence = 0;
            void add(float n, unsigned long long sequence) {
                if (attempts == 0) firstSequence = sequence;
                else if (sequence == lastSequence) ++repeatedFrames;
                lastSequence = sequence;
                ++attempts;
                if (norms.size() < 16) norms.push_back(n);
            }
            ~NormLog() {
                if (norms.empty()) return;
                wchar_t buf[16 * 8];
                size_t off = 0;
                for (size_t i = 0; i < norms.size() && off + 8 < ARRAYSIZE(buf); i++) {
                    off += static_cast<size_t>(swprintf(buf + off, 8, L"%.3f,", norms[i]));
                }
                if (off > 0 && buf[off - 1] == L',') buf[--off] = L'\0';
                FACELOGIN_INFO(L"Embedding norms pre-normalize (binding attempts): [%s]",
                               buf);
                FACELOGIN_INFO(L"Binding capture sequence: attempts=%llu, repeated=%llu, "
                               L"first=%llu, last=%llu",
                               attempts, repeatedFrames, firstSequence, lastSequence);
            }
        } normLog;

        const auto pipelineStart = std::chrono::steady_clock::now();
        bool firstFrameLogged = false;
        const auto grabFrame = [this, pipelineStart, &firstFrameLogged](
                                   FrameImage& frame, unsigned long long& frameSequence) {
            const bool grabbed = m_callbacks.grabFrame(frame, frameSequence);
            if (grabbed && !firstFrameLogged) {
                firstFrameLogged = true;
                FACELOGIN_INFO(L"Auth pipeline first valid frame %.1f ms after camera ready",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - pipelineStart).count());
            }
            return grabbed;
        };

        // Software-pipelined frame supply: while PAD(N) runs on its async
        // thread, the main thread grabs and detects frame N+1 into a single
        // prefetch slot, so PAD / embedding / binding IPC hide inside the
        // next frame's capture+detection time. Per-frame verdict logic and
        // ordering are unchanged; only the schedule moves.
        struct Prefetch {
            bool valid = false;
            FrameImage frame;                       // already rotated
            unsigned long long seq = 0;
            std::optional<OnnxDetector::Detection> det;
            std::chrono::steady_clock::time_point grabWall{};
        };

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
        Prefetch seed;
        FrameImage seedFrame;
        // The future joins on every exit, including cancellation. Declare it
        // after its input so that the frame outlives the async detector.
        std::future<std::optional<OnnxDetector::Detection>> seedFuture;
        unsigned long long seedSeq = 0;
        std::chrono::steady_clock::time_point seedWall{};

        // Replay the persisted last-good sensor combo BEFORE the warmup:
        // the driver intermittently forgets its manual UVC controls across
        // the idle power-down (2026-09-13), and without the replay every
        // such round starts at the driver default and pays the full
        // settle-loop tune. A replayed gain lands instantly; a replayed
        // exposure settles inside the warmup window. Equal values are never
        // re-written, so an intact driver pays nothing.
        bool replayWrote = false;
        const auto replayAt = std::chrono::steady_clock::now();
        const bool usingAutoExposure =
            PreferAutoExposureWithoutGain(sensorKnobs, &replayWrote);
        if (!usingAutoExposure && !m_config.tuneStatePath.empty()) {
            TuneKnobState saved;
            if (LoadTuneKnobState(m_config.tuneStatePath, saved)) {
                replayWrote = ApplyTuneKnobState(sensorKnobs, saved);
            }
        }

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
                return result;
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
            if (seedFuture.valid() &&
                seedFuture.wait_for(std::chrono::seconds(0))
                    == std::future_status::ready) {
                auto seedDet = seedFuture.get();
                seed = Prefetch{ true, std::move(seedFrame), seedSeq,
                                 std::move(seedDet), seedWall };
            }
            const bool gateOpened = warmup.Feed(MeanLuma(warmFrame));
            // A completed detection from the preceding sample already belongs
            // to this stable two-frame window. Keep it when AE and face luma
            // agree, rather than start another detection and move the first
            // counted frame's pacing anchor forward by one camera period.
            // An unstable/capped window, control write, missing face or older
            // sample keeps the original latest/pending detection path.
            bool reuseCompleted = gateOpened && warmup.Settled() &&
                usingAutoExposure && !replayWrote && hadPreviousWarm &&
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
            if (!seedFuture.valid() && !reuseCompleted) {
                seedFrame = warmFrame;               // luma already sampled; copy for detection
                RotateFrame(seedFrame, m_config.cameraRotation);
                seedSeq = seq;
                seedWall = std::chrono::steady_clock::now();
                seedFuture = std::async(std::launch::async, [this, &seedFrame] {
                    return m_detector.DetectLargestFace(seedFrame);
                });
            }
            if (gateOpened) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto gateAt = std::chrono::steady_clock::now();
        exposureTiming.gateReached = true;
        FACELOGIN_INFO(L"Exposure warmup: %d samples, %.0f ms%s",
                       warmup.Samples(),
                       std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - warmupStart).count(),
                       warmup.Settled() ? L" — stable" : L" — cap reached, proceeding");

        // Only the log consumes this snapshot. Read on the camera's current
        // COM thread while the outstanding SCRFD runs on its existing async
        // thread; do not add concurrent sensor calls or reuse it for tuning.
        long diagnosticExposure = 0, diagnosticGain = 0;
        bool diagnosticManual = false;
        bool haveDiagnosticExposure = false, haveDiagnosticGain = false;
        bool haveDiagnosticMode = false, diagnosticReadDone = false;
        const auto readDiagnosticKnobs = [&] {
            exposureTiming.phase = ExposureTimingProfile::Trigger;
            const auto queryAt = std::chrono::steady_clock::now();
            if (m_callbacks.readExposureState) {
                const auto exposureAt = std::chrono::steady_clock::now();
                const bool ok = m_callbacks.readExposureState(
                    diagnosticExposure, diagnosticManual);
                auto& c = exposureTiming.calls[ExposureTimingProfile::Trigger]
                                              [ExposureTimingProfile::ExposureState];
                const double ms = ElapsedMs(exposureAt);
                ++c.count;
                if (!ok) ++c.failed;
                c.totalMs += ms;
                c.maxMs = std::max(c.maxMs, ms);
                haveDiagnosticExposure = haveDiagnosticMode = ok;
            } else {
                haveDiagnosticExposure = sensorKnobs.exposureGet &&
                    sensorKnobs.exposureGet(diagnosticExposure);
            }
            haveDiagnosticGain = sensorKnobs.gainGet &&
                sensorKnobs.gainGet(diagnosticGain);
            if (!m_callbacks.readExposureState) {
                haveDiagnosticMode = sensorKnobs.exposureIsManual &&
                    sensorKnobs.exposureIsManual(diagnosticManual);
            }
            exposureTiming.triggerQueryMs = ElapsedMs(queryAt);
            diagnosticReadDone = true;
        };
        // If replay wrote a knob, defer the snapshot until after its existing
        // settle/re-measure path, so the log cannot describe pre-write state.
        if (!replayWrote && seedFuture.valid()) {
            readDiagnosticKnobs();
            exposureTiming.triggerQueriesBeforeSeedWait = true;
        }

        const auto seedWaitAt = std::chrono::steady_clock::now();
        if (seedFuture.valid()) {
            // Consume the same outstanding frame/detection, waiting only for
            // the work left after any early diagnostic queries.
            auto seedDet = seedFuture.get();
            seed = Prefetch{ true, std::move(seedFrame), seedSeq,
                             std::move(seedDet), seedWall };
        }
        exposureTiming.seedWaitMs = ElapsedMs(seedWaitAt);
        if (interrupted()) return result;
        exposureTiming.warmupSeedSeq = seed.valid ? seed.seq : 0;
        exposureTiming.warmupSeedAgeMs = seed.valid ? ElapsedMs(seed.grabWall) : -1.0;
        if (seed.valid && std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - seed.grabWall).count()
                              > kSeedMaxAgeMs) {
            seed = Prefetch{};                      // stale seed: grab fresh like the legacy loop
        }

        // Face-priority gain tune (face_gain_tune.h): the trigger measurement
        // reuses the seed's detection, so bright scenes — the common case —
        // pay one bbox luma pass and zero extra inference. In-band rounds
        // take the early return, which also refreshes the persisted combo;
        // only an actual tune invalidates the seed frame.
        FaceTuneRecovery tuneRecovery;
        GainTuneConfig tuneCfg;
        tuneCfg.persistPath = m_config.tuneStatePath;
        if (!tuneCfg.persistPath.empty()) tuneCfg.recovery = &tuneRecovery;
        const auto seedRefreshAt = std::chrono::steady_clock::now();
        if (!seed.valid && !interrupted()) {
            FrameImage fresh;
            unsigned long long freshSeq = 0;
            if (m_callbacks.grabFrame(fresh, freshSeq)) {
                RotateFrame(fresh, m_config.cameraRotation);
                auto det = m_detector.DetectLargestFace(fresh);
                seed = Prefetch{true, std::move(fresh), freshSeq, std::move(det),
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
                        return result;
                    }
                    if (m_callbacks.grabFrame(fresh, freshSeq)) {
                        lastObserved = std::max(lastObserved, freshSeq);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
                seed = Prefetch{};
                for (int attempt = 0; attempt < tuneCfg.maxGrabAttempts; ++attempt) {
                    if (interrupted()) {
                        return result;
                    }
                    if (m_callbacks.grabFrame(fresh, freshSeq) && freshSeq > lastObserved) {
                        RotateFrame(fresh, m_config.cameraRotation);
                        auto det = m_detector.DetectLargestFace(fresh);
                        // A lost face must replace the old detection too, so
                        // white-out recovery can run instead of using stale luma.
                        seed = Prefetch{true, std::move(fresh), freshSeq, std::move(det),
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
            if (!diagnosticReadDone) readDiagnosticKnobs();
            float triggerLuma = -1.0f;
            if (seed.valid && seed.det) triggerLuma = FaceBoxLuma(seed.frame, *seed.det);
            wchar_t knobsText[96] = L"knobs n/a";
            if (haveDiagnosticExposure || haveDiagnosticGain) {
                wchar_t expPart[48] = L"exposure n/a";
                wchar_t gainPart[40] = L"gain n/a";
                if (haveDiagnosticExposure) {
                    _snwprintf_s(expPart, _TRUNCATE, L"exposure %ld (%s)", diagnosticExposure,
                                 haveDiagnosticMode ? (diagnosticManual ? L"manual" : L"auto") : L"mode n/a");
                }
                if (haveDiagnosticGain) _snwprintf_s(gainPart, _TRUNCATE, L"gain %ld", diagnosticGain);
                _snwprintf_s(knobsText, _TRUNCATE, L"%s, %s", expPart, gainPart);
            }
            FACELOGIN_INFO(L"Exposure tune trigger: %s, face luma %.0f — %s",
                           seed.valid ? (seed.det ? L"face" : L"no face")
                                      : L"no fresh frame",
                           triggerLuma, knobsText);
        }
        exposureTiming.triggerMs = ElapsedMs(triggerAt);
        exposureTiming.phase = ExposureTimingProfile::Tune;
        const auto tuneAt = std::chrono::steady_clock::now();
        bool darkRecoveryAttempted = false;
        if (seed.valid && seed.det) {
            const float faceLuma = FaceBoxLuma(seed.frame, *seed.det);
            if (faceLuma >= 0.0f) {
                const int tuneSteps = TuneFaceExposure(
                    faceLuma, sensorKnobs, m_callbacks.grabFrame,
                    m_detector, interrupted,
                    m_config.cameraRotation, tuneCfg);
                if (tuneSteps > 0) seed = Prefetch{};
            }
        } else if (seed.valid &&
                   (IsSceneWhiteout(seed.frame) || IsSceneDark(seed.frame, tuneCfg))) {
            // Recovery precedes all PAD/identity frames. Never reuse a seed
            // after this path: controls or the camera stream may have moved.
            darkRecoveryAttempted = IsSceneDark(seed.frame, tuneCfg);
            TuneFaceExposure(sensorKnobs, m_callbacks.grabFrame,
                             m_detector, interrupted,
                             m_config.cameraRotation, tuneCfg);
            seed = Prefetch{};
        }
        exposureTiming.tuneMs = ElapsedMs(tuneAt);
        if (interrupted()) return result;

        const auto statusAt = std::chrono::steady_clock::now();
        m_callbacks.reportStatus(L"正在识别...");
        exposureTiming.statusMs = ElapsedMs(statusAt);
        exposureTiming.gateToLoopMs = ElapsedMs(gateAt);
        exposureTiming.phase = ExposureTimingProfile::Loop;
        // No per-round echo of the PAD threshold/check counts here: the
        // active config (threshold included) is logged once per load by
        // LoadConfig, and "need N" appears in the failure line.
        const int totalChecks = AntiSpoofCheckCount(m_config.fastUnlock);
        const int passRequired = AntiSpoofPassRequired(totalChecks);

        const auto livenessStart = std::chrono::steady_clock::now();
        LivenessTiming timing{livenessStart};
        int totalChecked = 0;
        unsigned int bindingCount = 0;
        bool havePrevRect = false;
        FaceRect prevRect{};
        bool livenessInferenceError = false;
        bool identityRejected = false;
        bool identityRetryTimedOut = false;
        bool failFastEmpty = false;
        bool failFastAttack = false;
        // Both verifyBinding hosts return Retry only for "no enrolled identity
        // matches this face". A face that keeps missing every enrolled identity
        // never leaves the anchor frame, so no frame is ever counted and the
        // persistent-attack timer fires without a single PAD verdict — that
        // outcome is an unknown face, not a PAD failure, and must not report
        // the misleading liveness message.
        bool sawUnknownFace = false;
        std::wstring identityError;
        // Capture conditions for the outcome log lines: the first identity
        // miss (what the unknown-face failure looked like) and the latest
        // accepted binding (what a passing frame looked like).
        long retryFaceWidth = 0;
        float retryFaceLuma = -1.0f;
        bool haveRetryConditions = false;
        long boundFaceWidth = 0;
        float boundFaceLuma = -1.0f;

        // Pacing invariant (explicit, auditable, stricter than the legacy
        // implicit "sleep 60 + processing time"): consecutive COUNTED frames
        // are grabbed >= 60 ms apart with strictly increasing camera sequence
        // numbers. Discarded frames (empty embedding / Retry / IoU skip) never
        // move the anchor, so later grabs automatically satisfy the guard.
        std::chrono::steady_clock::time_point lastCountedWall{};
        unsigned long long lastCountedSeq = 0;
        bool haveLastCounted = false;
        double minCountedGrabIntervalMs = 0.0;
        bool haveMinInterval = false;

        // Pre-binding exposure correction budget (2026-09-19 17:45:46
        // post-mortem): the pre-loop tune needs a face to measure, so a
        // trigger frame without one (late walk-up, dark-face miss) or
        // brightness that drifts after it left the loop matching a dark chip
        // until the attack timer killed the round — identity distance 0.989
        // at face luma 18. While NO frame has been counted yet, nothing
        // contributes to PAD or identity evidence, so sensor writes are still
        // safe; an out-of-band face luma earns one full tune pass (the gate
        // lives just below, before the first binding attempt). Two passes
        // bound the worst case; a face that stays out of band after them
        // proceeds exactly like the legacy loop.
        int lateTuneBudget = 2;
        const bool sensorKnobsWired = !usingAutoExposure &&
            (static_cast<bool>(sensorKnobs.exposureSet) ||
             static_cast<bool>(sensorKnobs.gainSet));

        // Returns false on interruption, window expiry or grab failure.
        // The caller re-checks interrupted() to preserve timeout/cancel results.
        // Failed grabs keep the legacy 30 ms
        // retry pace outside this helper's loop.
        const auto grabWithPacing = [&](Prefetch& out) {
            for (;;) {
                if (interrupted() || m_callbacks.isClientDisconnected()) return false;
                // A stalled camera can keep returning the last buffered frame.
                // Keep the outer loop's deadlines active during dedup/pacing.
                const auto waitNow = std::chrono::steady_clock::now();
                if (timing.WindowExpired(waitNow)) {
                    return false;
                }
                if (haveLastCounted) {
                    const auto deadline = lastCountedWall + std::chrono::milliseconds(60);
                    if (std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_until(deadline);
                        continue;                   // re-check cancel after waking
                    }
                }
                FrameImage frame;
                unsigned long long seq = 0;
                if (!grabFrame(frame, seq)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    return false;
                }
                // Defensive dedup: the 60 ms guard guarantees a fresh 30 fps
                // frame mathematically, but a stale buffered frame would
                // silently violate the strictly-increasing-sequence invariant.
                if (haveLastCounted && seq <= lastCountedSeq) {
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
                RotateFrame(frame, m_config.cameraRotation);
                auto det = m_detector.DetectLargestFace(frame);
                out = Prefetch{ true, std::move(frame), seq, std::move(det), grabWall };
                return true;
            }
        };

        Prefetch next = std::move(seed);            // warmup SCRFD pre-run seed, if fresh
        while (totalChecked < totalChecks) {
            if (interrupted()) {
                return result;
            }
            if (m_callbacks.isClientDisconnected()) {
                FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
                result.cancelled = true;
                return result;
            }

            const auto now = std::chrono::steady_clock::now();
            if (timing.WindowExpired(now)) {
                break;
            }
            if (timing.ShouldFailEmptyScene(now)) {
                FACELOGIN_INFO(L"No face detected within 2.5s — failing fast (empty scene)");
                failFastEmpty = true;
                break;
            }

            if (!next.valid) {
                if (!grabWithPacing(next)) {
                    if (interrupted()) {
                        return result;
                    }
                    continue;                       // grab failure: legacy 30 ms pace already served
                }
            }
            Prefetch cur = std::move(next);
            next = Prefetch{};

            if (!cur.det) {
                if (totalChecked == 0 && sensorKnobsWired && !darkRecoveryAttempted &&
                    IsSceneDark(cur.frame, tuneCfg)) {
                    darkRecoveryAttempted = true;
                    timing.Suspend(std::chrono::steady_clock::now());
                    TuneFaceExposure(sensorKnobs, m_callbacks.grabFrame, m_detector,
                                     interrupted, m_config.cameraRotation, tuneCfg);
                    timing.Resume(std::chrono::steady_clock::now());
                    if (interrupted()) return result;
                    continue; // discard all pre-write/no-face evidence
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            const FaceRect faceRect(static_cast<long>(cur.det->x1),
                                    static_cast<long>(cur.det->y1),
                                    static_cast<long>(cur.det->x2),
                                    static_cast<long>(cur.det->y2));

            // Resume only interrupted sensor writes, before any frame counts.
            // The independent gate consumes fresh frames and the original
            // deadline; the old trigger is discarded even when it fails.
            if (totalChecked == 0 && tuneRecovery.pending && !tuneRecovery.attempted) {
                const float luma = FaceRegionLuma(cur.frame, faceRect);
                if (luma >= tuneCfg.minFaceLuma && luma <= tuneCfg.maxFaceLuma) {
                    timing.Suspend(std::chrono::steady_clock::now());
                    ConfirmPendingFaceTune(sensorKnobs, m_callbacks.grabFrame, m_detector,
                        [&] { return interrupted() || m_callbacks.isClientDisconnected(); },
                        m_config.cameraRotation, cur.seq, tuneCfg);
                    timing.Resume(std::chrono::steady_clock::now());
                    if (interrupted()) return result;
                    continue;
                }
            }

            // The correction gate sits BEFORE the first binding attempt and
            // before the first counted frame: sensor parameters must never
            // change once frames contribute to PAD/identity evidence. It also
            // precedes OnFaceDetected, so a face that arrives dark does not
            // arm the 2 s attack clock against the tune that is rescuing it.
            if (totalChecked == 0 && lateTuneBudget > 0 && sensorKnobsWired) {
                const float lateLuma = FaceRegionLuma(cur.frame, faceRect);
                if (lateLuma >= 0.0f &&
                    (lateLuma < tuneCfg.minFaceLuma || lateLuma > tuneCfg.maxFaceLuma)) {
                    --lateTuneBudget;
                    timing.Suspend(std::chrono::steady_clock::now());
                    FACELOGIN_INFO(L"Late exposure correction: face luma %.0f out of band "
                                   L"[%.0f–%.0f] before first counted frame — tuning "
                                   L"(budget left %d)",
                                   lateLuma, tuneCfg.minFaceLuma, tuneCfg.maxFaceLuma,
                                   lateTuneBudget);
                    // The triggering frame is discarded after the tune: the
                    // knobs and the camera stream may both have moved. The
                    // pacing anchor is untouched — no frame was counted yet.
                    TuneFaceExposure(lateLuma, sensorKnobs,
                                     m_callbacks.grabFrame, m_detector,
                                     interrupted, m_config.cameraRotation,
                                     tuneCfg);
                    timing.Resume(std::chrono::steady_clock::now());
                    if (interrupted()) {
                        return result;
                    }
                    continue;
                }
            }

            const auto faceNow = std::chrono::steady_clock::now();
            timing.OnFaceDetected(faceNow);

            if (timing.ShouldFailPersistentAttack(faceNow)) {
                if (totalChecked == 0 && sawUnknownFace) {
                    FACELOGIN_INFO(L"Face present but no enrolled identity matched for 2s — "
                                   L"failing fast (unknown face; width=%ld px, face luma=%.0f)",
                                   retryFaceWidth, retryFaceLuma);
                } else {
                    FACELOGIN_INFO(L"PAD persistently below threshold for 2s — failing fast (likely attack)");
                }
                failFastAttack = true;
                break;
            }

            // Default plan: anchor frames are the first bound frame (locks
            // the SID) and the last counted frame (tail anchor), with the
            // counted-3 consensus frame between them — bindings land on
            // counted frames 1/3/5. Fast unlock binds EVERY counted frame,
            // so 3 frames carry all 3 bindings (required by the pass
            // condition below and by the parent's 3-probe exchange
            // validator) and the IoU-only path never runs.
            const bool isAnchor = m_config.fastUnlock || bindingCount == 0 ||
                                  totalChecked == totalChecks - 1;
            const bool isConsensusFrame = totalChecked == 2;

            auto scoreFuture = std::async(std::launch::async, [&cur, &faceRect, this] {
                return m_antiSpoof.Predict(cur.frame, faceRect);
            });

            if (isAnchor || isConsensusFrame) {
                float preNorm = 0.0f;
                auto embedding = m_recognizer.ComputeEmbedding(cur.frame, cur.det->kps,
                                                               &preNorm);
                if (embedding.empty()) {
                    scoreFuture.get();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }
                normLog.add(preNorm, cur.seq);

                const BindingDecision decision =
                    m_callbacks.verifyBinding(embedding, bindingCount, preNorm,
                                              EstimateYawDeg(cur.det->kps),
                                              EstimatePitchDeg(cur.det->kps));
                if (decision.kind == BindingDecisionKind::Retry) {
                    scoreFuture.get();
                    if (!haveRetryConditions) {
                        retryFaceWidth = faceRect.width();
                        retryFaceLuma = FaceRegionLuma(cur.frame, faceRect);
                        haveRetryConditions = true;
                    }
                    sawUnknownFace = true;
                    if (timing.OnIdentityRetry(std::chrono::steady_clock::now())) {
                        FACELOGIN_INFO(L"Identity matching failed continuously for 2s — "
                                       L"failing fast (bindings=%u/3, counted=%d, "
                                       L"width=%ld px, face luma=%.0f)",
                                       bindingCount, totalChecked, faceRect.width(),
                                       FaceRegionLuma(cur.frame, faceRect));
                        identityRetryTimedOut = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }
                if (decision.kind == BindingDecisionKind::Reject) {
                    scoreFuture.get();
                    identityRejected = true;
                    identityError = decision.errorMessage.empty()
                        ? L"活体验证期间人脸不匹配，请重试"
                        : decision.errorMessage;
                    break;
                }
                timing.OnIdentityAccepted();
                ++bindingCount;
                boundFaceWidth = faceRect.width();
                boundFaceLuma = FaceRegionLuma(cur.frame, faceRect);
            } else if (havePrevRect) {
                const auto interW = std::max<long>(0,
                    std::min(faceRect.right(), prevRect.right()) -
                    std::max(faceRect.left(), prevRect.left()) + 1);
                const auto interH = std::max<long>(0,
                    std::min(faceRect.bottom(), prevRect.bottom()) -
                    std::max(faceRect.top(), prevRect.top()) + 1);
                const double inter = static_cast<double>(interW) * interH;
                const double uni = static_cast<double>(faceRect.width()) * faceRect.height() +
                    static_cast<double>(prevRect.width()) * prevRect.height() - inter;
                if (uni > 0.0 && inter / uni < 0.35) {
                    scoreFuture.get();
                    FACELOGIN_WARN(L"Face position discontinuity during anti-spoof — frame skipped");
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }
            }

            // Advance the pacing anchor to the frame being consumed BEFORE
            // prefetching N+1: the guard for N+1 must measure from frame N,
            // not the stale N-1 anchor. Optimistic update is safe because
            // every path from here that skips counting frame N (invalid PAD
            // score, cancel) terminates the loop outright.
            if (haveLastCounted) {
                const double intervalMs = std::chrono::duration<double, std::milli>(
                    cur.grabWall - lastCountedWall).count();
                if (!haveMinInterval || intervalMs < minCountedGrabIntervalMs) {
                    minCountedGrabIntervalMs = intervalMs;
                    haveMinInterval = true;
                }
            }
            lastCountedWall = cur.grabWall;
            lastCountedSeq = cur.seq;
            haveLastCounted = true;

            // Overlap window: PAD(N) is running while the main thread
            // produces frame N+1's capture+detection. The +1 accounts for
            // this frame being counted as totalChecked+1; skip prefetch when
            // it would be the last frame. A wasted prefetch (this frame later
            // fails on an invalid PAD score) costs CPU only, never latency.
            if (totalChecked + 1 < totalChecks) {
                Prefetch candidate;
                if (grabWithPacing(candidate)) {
                    next = std::move(candidate);
                } else if (interrupted()) {
                    // Cancellation or timeout must abort before frame N is counted.
                    scoreFuture.get();
                    return result;
                }
                // Grab failure: leave the slot empty — frame N still counts
                // below, and the next iteration grabs fresh at loop pace.
            }

            const float score = scoreFuture.get();
            if (interrupted()) return result;
            if (!std::isfinite(score) || score < 0.0f || score > 1.0f) {
                FACELOGIN_ERROR(L"Anti-spoof inference returned invalid score: %.4f", score);
                livenessInferenceError = true;
                break;
            }

            ++totalChecked;
            prevRect = faceRect;
            havePrevRect = true;
            if (score >= m_config.antiSpoofThreshold) {
                timing.OnPadScore(true);
            }
            FACELOGIN_DEBUG(L"Anti-spoof frame %d: score=%.3f (pass=%d)",
                            totalChecked, score, timing.PassCount());
        }

        if (haveMinInterval) {
            FACELOGIN_INFO(L"Anti-spoof frame pacing: min grab interval %.1f ms over %d counted frames (guard 60 ms)",
                           minCountedGrabIntervalMs, totalChecked);
        }

        const bool livenessPassed = !failFastEmpty && !failFastAttack &&
            !livenessInferenceError && !identityRejected && !identityRetryTimedOut &&
            totalChecked == totalChecks && timing.PassCount() >= passRequired && bindingCount == 3;
        if (livenessPassed) {
            FACELOGIN_INFO(L"Liveness passed — tail anchor bound, authentication evidence "
                           L"complete (width=%ld px, face luma=%.0f)",
                           boundFaceWidth, boundFaceLuma);
            result.succeeded = true;
            return result;
        }

        FACELOGIN_WARN(L"Authentication evidence incomplete: identity=%u/3, "
                       L"PAD=%d/%d passed (need %d)",
                       bindingCount, timing.PassCount(), totalChecked, passRequired);
        if (livenessInferenceError) {
            result.errorMessage = L"活体检测模块异常，请使用密码登录";
        } else if (identityRejected) {
            result.errorMessage = identityError;
        } else if (!timing.AnyFaceSeen()) {
            result.errorMessage = L"未检测到人脸";
        } else if (identityRetryTimedOut || (totalChecked == 0 && sawUnknownFace) ||
                   (timing.IdentityRetryPending() && bindingCount < 3 &&
                    timing.PassCount() == totalChecked)) {
            result.errorMessage = bindingCount == 0
                ? L"未识别到已注册人脸，请使用密码登录"
                : L"人脸匹配持续失败，请调整位置或光照后重试";
        } else {
            result.errorMessage = L"未通过活体检测，请使用真实人脸";
        }
        return result;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"AuthPipeline threw: %hs", e.what());
        result.errorMessage = L"认证过程异常，请使用密码登录";
        return result;
    }
}

} // namespace facelogin
