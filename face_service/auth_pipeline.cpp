#include "auth_pipeline.h"
#include "auth_pipeline_diagnostics.h"
#include "auth_frame_preparation.h"
#include "auth_frame_stream.h"

#include "../common/logger.h"
#include "../vision/face_align.h"
#include "../vision/face_gain_tune.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>
#include <utility>

namespace facelogin {
using auth_diagnostics::ElapsedMs;
using auth_diagnostics::ExposureTimingProfile;
using auth_diagnostics::NormLog;
using auth_diagnostics::ProfileSensorKnobs;

// Pacing between discarded auth frames (no face, bad grab, retry). This is
// the legacy 30 ms grab-loop pace and counts against the shared 5 s auth
// budget; changing it invalidates the measured unlock baselines.
static constexpr auto kDiscardRetryPace = std::chrono::milliseconds(30);

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

// IoU gate between consecutive counted frames: a face that teleports between
// frame N-1 and N breaks the PAD evidence chain (different skin, different
// distance), so the frame is discarded instead of counted.
static bool PositionDiscontinuous(const FaceRect& faceRect, const FaceRect& prevRect) {
    const auto interW = std::max<long>(0,
        std::min(faceRect.right(), prevRect.right()) -
        std::max(faceRect.left(), prevRect.left()) + 1);
    const auto interH = std::max<long>(0,
        std::min(faceRect.bottom(), prevRect.bottom()) -
        std::max(faceRect.top(), prevRect.top()) + 1);
    const double inter = static_cast<double>(interW) * interH;
    const double uni = static_cast<double>(faceRect.width()) * faceRect.height() +
        static_cast<double>(prevRect.width()) * prevRect.height() - inter;
    return uni > 0.0 && inter / uni < 0.35;
}

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
            result.outcome = AuthOutcome::Cancelled;
            return true;
        }
        if (ipc::RemainingAuthTimeMs(authDeadline) == 0) {
            result.outcome = AuthOutcome::TimedOut;
            return true;
        }
        return false;
    };
    if (interrupted()) return result;

    try {
        ExposureTimingProfile exposureTiming;
        const SensorKnobs sensorKnobs = ProfileSensorKnobs(m_callbacks.sensorKnobs, exposureTiming);
        NormLog normLog;

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

        PreparedAuthFrames prepared;
        if (!PrepareFrames(prepared, exposureTiming, sensorKnobs, interrupted, grabFrame)) {
            PersistFailedRoundTuneProgress(prepared);
            return result;
        }

        const auto statusAt = std::chrono::steady_clock::now();
        m_callbacks.reportStatus(L"正在识别...");
        exposureTiming.statusMs = ElapsedMs(statusAt);
        exposureTiming.gateToLoopMs = ElapsedMs(prepared.gateAt);
        exposureTiming.phase = ExposureTimingProfile::Loop;
        // No per-round echo of the PAD threshold/check counts here: the
        // active config (threshold included) is logged once per load by
        // LoadConfig, and "need N" appears in the failure line.

        AuthLoopState state{std::chrono::steady_clock::now()};
        state.sensorKnobsWired = !prepared.usingAutoExposure &&
            (static_cast<bool>(sensorKnobs.exposureSet) ||
             static_cast<bool>(sensorKnobs.gainSet));
        AuthFrameStream frames(m_detector, m_config.cameraRotation, grabFrame, [&] {
            return interrupted() || m_callbacks.isClientDisconnected() ||
                state.timing.WindowExpired(std::chrono::steady_clock::now());
        });

        const AuthLoopStop stop = RunLivenessLoop(state, frames, prepared, sensorKnobs,
                                                  interrupted, normLog);
        if (stop == AuthLoopStop::ClientDisconnected) result.outcome = AuthOutcome::Cancelled;
        if (stop == AuthLoopStop::Interrupted || stop == AuthLoopStop::ClientDisconnected) {
            PersistFailedRoundTuneProgress(prepared);
            return result;
        }

        if (const auto minInterval = frames.MinimumIntervalMs()) {
            FACELOGIN_INFO(L"Anti-spoof frame pacing: min grab interval %.1f ms over %d counted frames (guard 60 ms)",
                           *minInterval, state.totalChecked);
        }
        AuthPipelineResult final = ResolveOutcome(stop, state);
        if (final.outcome != AuthOutcome::Succeeded) PersistFailedRoundTuneProgress(prepared);
        return final;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"AuthPipeline threw: %hs", e.what());
        result.errorMessage = L"认证过程异常，请使用密码登录";
        return result;
    }
}

// Per-iteration pre-frame gates: cancellation, disconnect, the 8 s PAD window
// and the 2.5 s empty-scene fast fail. nullopt = keep going.
std::optional<AuthLoopStop> AuthPipeline::CheckLoopGates(
    AuthLoopState& state, const std::function<bool()>& interrupted) {
    if (interrupted()) return AuthLoopStop::Interrupted;
    if (m_callbacks.isClientDisconnected()) {
        FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
        return AuthLoopStop::ClientDisconnected;
    }
    const auto now = std::chrono::steady_clock::now();
    if (state.timing.WindowExpired(now)) return AuthLoopStop::WindowExpired;
    if (state.timing.ShouldFailEmptyScene(now)) {
        FACELOGIN_INFO(L"No face detected within 2.5s — failing fast (empty scene)");
        return AuthLoopStop::EmptyScene;
    }
    return std::nullopt;
}

// The counted-frame anti-spoof loop: PAD inference on frame N overlapped with
// capture+detection of frame N+1, identity bindings on the planned frames.
// Returns how it ended; Interrupted/ClientDisconnected leave the terminal
// outcome to Run (already written by the interrupted lambda).
AuthLoopStop AuthPipeline::RunLivenessLoop(AuthLoopState& state,
                                           AuthFrameStream& frames,
                                           PreparedAuthFrames& prepared,
                                           const SensorKnobs& sensorKnobs,
                                           const std::function<bool()>& interrupted,
                                           auth_diagnostics::NormLog& normLog) {
    const int totalChecks = AntiSpoofCheckCount(m_config.fastUnlock);
    AuthFrame next = std::move(prepared.seed);    // warmup SCRFD pre-run seed, if fresh
    while (state.totalChecked < totalChecks) {
        if (const auto stop = CheckLoopGates(state, interrupted)) return *stop;

        if (!next.valid) {
            if (!frames.Grab(next)) {
                if (interrupted()) return AuthLoopStop::Interrupted;
                continue;                       // grab failure: legacy 30 ms pace already served
            }
        }
        AuthFrame cur = std::move(next);
        next = AuthFrame{};

        if (state.totalChecked == 0) {
            const auto correction = CorrectExposureBeforeFirstCount(
                state, prepared, cur, sensorKnobs, interrupted);
            if (correction == PreCountCorrection::Interrupted)
                return AuthLoopStop::Interrupted;
            if (correction == PreCountCorrection::Corrected) continue;
        }
        if (!cur.det) {
            std::this_thread::sleep_for(kDiscardRetryPace);
            continue;
        }

        const FaceRect faceRect(static_cast<long>(cur.det->x1),
                                static_cast<long>(cur.det->y1),
                                static_cast<long>(cur.det->x2),
                                static_cast<long>(cur.det->y2));
        // Latest observed face luma feeds the failed-round progress rule: a
        // face that appears after a no-face recovery is the only proof the
        // climb reached anything photographable.
        prepared.lastFaceLuma = FaceRegionLuma(cur.frame, faceRect);

        const auto faceNow = std::chrono::steady_clock::now();
        state.timing.OnFaceDetected(faceNow);
        if (state.timing.ShouldFailPersistentAttack(faceNow)) {
            if (state.totalChecked == 0 && state.sawUnknownFace) {
                FACELOGIN_INFO(L"Face present but no enrolled identity matched for 2s — "
                               L"failing fast (unknown face; width=%ld px, face luma=%.0f)",
                               state.retryFaceWidth, state.retryFaceLuma);
            } else {
                FACELOGIN_INFO(L"PAD persistently below threshold for 2s — failing fast (likely attack)");
            }
            return AuthLoopStop::Attack;
        }

        // Default plan: anchor frames are the first bound frame (locks
        // the SID) and the last counted frame (tail anchor), with the
        // counted-3 consensus frame between them — bindings land on
        // counted frames 1/3/5. Fast unlock binds EVERY counted frame,
        // so 3 frames carry all 3 bindings (required by the pass
        // condition in ResolveOutcome and by the parent's 3-probe
        // exchange validator) and the IoU-only path never runs.
        const bool isAnchor = m_config.fastUnlock || state.bindingCount == 0 ||
                              state.totalChecked == totalChecks - 1;
        const bool isConsensusFrame = state.totalChecked == 2;

        auto scoreFuture = std::async(std::launch::async, [&cur, &faceRect, this] {
            return m_antiSpoof.Predict(cur.frame, faceRect);
        });

        if (isAnchor || isConsensusFrame) {
            switch (AttemptBinding(state, cur, faceRect, scoreFuture, normLog)) {
            case BindingDisposition::Bound:
                break;
            case BindingDisposition::Discard:
                continue;
            case BindingDisposition::RetryTimedOut:
                return AuthLoopStop::IdentityRetryTimedOut;
            case BindingDisposition::Rejected:
                return AuthLoopStop::IdentityRejected;
            }
        } else if (state.havePrevRect &&
                   PositionDiscontinuous(faceRect, state.prevRect)) {
            scoreFuture.get();
            FACELOGIN_WARN(L"Face position discontinuity during anti-spoof — frame skipped");
            std::this_thread::sleep_for(kDiscardRetryPace);
            continue;
        }

        // Advance the pacing anchor to the frame being consumed BEFORE
        // prefetching N+1: the guard for N+1 must measure from frame N,
        // not the stale N-1 anchor. Optimistic update is safe because
        // every path from here that skips counting frame N (invalid PAD
        // score, cancel) terminates the loop outright.
        frames.AdvanceAnchor(cur);

        if (const auto stop = PrefetchNextFrame(state, frames, next, scoreFuture,
                                                totalChecks, interrupted))
            return *stop;
        if (const auto stop = CountPadFrame(state, scoreFuture, faceRect, interrupted))
            return *stop;
    }
    return AuthLoopStop::CountingComplete;
}

// Overlap window: PAD(N) is running while the main thread produces frame
// N+1's capture+detection. The +1 accounts for this frame being counted as
// totalChecked+1; skip prefetch when it would be the last frame. A wasted
// prefetch (this frame later fails on an invalid PAD score) costs CPU only,
// never latency.
std::optional<AuthLoopStop> AuthPipeline::PrefetchNextFrame(
    AuthLoopState& state, AuthFrameStream& frames, AuthFrame& next,
    std::future<float>& scoreFuture, int totalChecks,
    const std::function<bool()>& interrupted) {
    if (state.totalChecked + 1 >= totalChecks) return std::nullopt;
    AuthFrame candidate;
    if (frames.Grab(candidate)) {
        next = std::move(candidate);
    } else if (interrupted()) {
        // Cancellation or timeout must abort before frame N is counted.
        scoreFuture.get();
        return AuthLoopStop::Interrupted;
    }
    // Grab failure: leave the slot empty — frame N still counts, and the
    // next iteration grabs fresh at loop pace.
    return std::nullopt;
}

// The three pre-first-count exposure corrections, all gated on "no frame has
// been counted yet": dark-scene recovery (no face to measure), the pending
// interrupted-tune confirmation, and the out-of-band face-luma correction.
// A corrected round discards its triggering frame — the knobs and the camera
// stream may both have moved, so the caller grabs fresh. Only called while
// totalChecked == 0.
PreCountCorrection AuthPipeline::CorrectExposureBeforeFirstCount(
    AuthLoopState& state, PreparedAuthFrames& prepared, const AuthFrame& cur,
    const SensorKnobs& sensorKnobs, const std::function<bool()>& interrupted) {
    auto& tuneCfg = prepared.tuneConfig;
    if (!cur.det) {
        if (state.sensorKnobsWired && !prepared.darkRecoveryAttempted &&
            IsSceneDark(cur.frame, tuneCfg)) {
            prepared.darkRecoveryAttempted = true;
            state.timing.Suspend(std::chrono::steady_clock::now());
            TuneFaceExposure(sensorKnobs, m_callbacks.grabFrame, m_detector,
                             interrupted, m_config.cameraRotation, tuneCfg);
            state.timing.Resume(std::chrono::steady_clock::now());
            // All pre-write/no-face evidence is discarded with the frame.
            return interrupted() ? PreCountCorrection::Interrupted
                                 : PreCountCorrection::Corrected;
        }
        return PreCountCorrection::NotNeeded;
    }

    const FaceRect faceRect(static_cast<long>(cur.det->x1),
                            static_cast<long>(cur.det->y1),
                            static_cast<long>(cur.det->x2),
                            static_cast<long>(cur.det->y2));

    // Resume only interrupted sensor writes, before any frame counts.
    // The independent gate consumes fresh frames and the original
    // deadline; the old trigger is discarded even when it fails.
    if (prepared.tuneRecovery.pending && !prepared.tuneRecovery.attempted) {
        const float luma = FaceRegionLuma(cur.frame, faceRect);
        if (luma >= tuneCfg.minFaceLuma && luma <= tuneCfg.maxFaceLuma) {
            state.timing.Suspend(std::chrono::steady_clock::now());
            ConfirmPendingFaceTune(sensorKnobs, m_callbacks.grabFrame, m_detector,
                [&] { return interrupted() || m_callbacks.isClientDisconnected(); },
                m_config.cameraRotation, cur.seq, tuneCfg);
            state.timing.Resume(std::chrono::steady_clock::now());
            return interrupted() ? PreCountCorrection::Interrupted
                                 : PreCountCorrection::Corrected;
        }
    }

    // The correction gate sits BEFORE the first binding attempt and
    // before the first counted frame: sensor parameters must never
    // change once frames contribute to PAD/identity evidence. It also
    // precedes OnFaceDetected, so a face that arrives dark does not
    // arm the 2 s attack clock against the tune that is rescuing it.
    if (state.lateTuneBudget > 0 && state.sensorKnobsWired) {
        const float lateLuma = FaceRegionLuma(cur.frame, faceRect);
        if (lateLuma >= 0.0f &&
            (lateLuma < tuneCfg.minFaceLuma || lateLuma > tuneCfg.maxFaceLuma)) {
            --state.lateTuneBudget;
            state.timing.Suspend(std::chrono::steady_clock::now());
            FACELOGIN_INFO(L"Late exposure correction: face luma %.0f out of band "
                           L"[%.0f–%.0f] before first counted frame — tuning "
                           L"(budget left %d)",
                           lateLuma, tuneCfg.minFaceLuma, tuneCfg.maxFaceLuma,
                           state.lateTuneBudget);
            // The triggering frame is discarded after the tune: the
            // knobs and the camera stream may both have moved. The
            // pacing anchor is untouched — no frame was counted yet.
            TuneFaceExposure(lateLuma, sensorKnobs,
                             m_callbacks.grabFrame, m_detector,
                             interrupted, m_config.cameraRotation,
                             tuneCfg);
            state.timing.Resume(std::chrono::steady_clock::now());
            return interrupted() ? PreCountCorrection::Interrupted
                                 : PreCountCorrection::Corrected;
        }
    }
    return PreCountCorrection::NotNeeded;
}

// Identity binding on an anchor or consensus frame: embedding, then the
// parent's binding verdict. Every path that does not keep counting the frame
// joins the in-flight PAD inference (scoreFuture) before returning — the
// async task captures the frame by reference and the caller destroys it on
// the next iteration.
BindingDisposition AuthPipeline::AttemptBinding(AuthLoopState& state,
                                                const AuthFrame& cur,
                                                const FaceRect& faceRect,
                                                std::future<float>& scoreFuture,
                                                auth_diagnostics::NormLog& normLog) {
    float preNorm = 0.0f;
    auto embedding = m_recognizer.ComputeEmbedding(cur.frame, cur.det->kps,
                                                   &preNorm);
    if (embedding.empty()) {
        scoreFuture.get();
        std::this_thread::sleep_for(kDiscardRetryPace);
        return BindingDisposition::Discard;
    }
    normLog.add(preNorm, cur.seq);

    const BindingDecision decision =
        m_callbacks.verifyBinding(embedding, state.bindingCount, preNorm,
                                  EstimateYawDeg(cur.det->kps),
                                  EstimatePitchDeg(cur.det->kps));
    if (decision.kind == BindingDecisionKind::Retry) {
        scoreFuture.get();
        if (!state.haveRetryConditions) {
            state.retryFaceWidth = faceRect.width();
            state.retryFaceLuma = FaceRegionLuma(cur.frame, faceRect);
            state.haveRetryConditions = true;
        }
        state.sawUnknownFace = true;
        if (state.timing.OnIdentityRetry(std::chrono::steady_clock::now())) {
            FACELOGIN_INFO(L"Identity matching failed continuously for 2s — "
                           L"failing fast (bindings=%u/3, counted=%d, "
                           L"width=%ld px, face luma=%.0f)",
                           state.bindingCount, state.totalChecked, faceRect.width(),
                           FaceRegionLuma(cur.frame, faceRect));
            return BindingDisposition::RetryTimedOut;
        }
        std::this_thread::sleep_for(kDiscardRetryPace);
        return BindingDisposition::Discard;
    }
    if (decision.kind == BindingDecisionKind::Reject) {
        scoreFuture.get();
        state.identityError = decision.errorMessage.empty()
            ? L"活体验证期间人脸不匹配，请重试"
            : decision.errorMessage;
        return BindingDisposition::Rejected;
    }
    state.timing.OnIdentityAccepted();
    ++state.bindingCount;
    state.boundFaceWidth = faceRect.width();
    state.boundFaceLuma = FaceRegionLuma(cur.frame, faceRect);
    return BindingDisposition::Bound;
}

// Waits for the overlapped PAD verdict on a frame that survived identity and
// continuity, then counts it. Returns nullopt when the frame was counted;
// Interrupted (outcome already set) and PadInferenceError are terminal.
std::optional<AuthLoopStop> AuthPipeline::CountPadFrame(
    AuthLoopState& state, std::future<float>& scoreFuture, const FaceRect& faceRect,
    const std::function<bool()>& interrupted) {
    const float score = scoreFuture.get();
    if (interrupted()) return AuthLoopStop::Interrupted;
    if (!std::isfinite(score) || score < 0.0f || score > 1.0f) {
        FACELOGIN_ERROR(L"Anti-spoof inference returned invalid score: %.4f", score);
        return AuthLoopStop::PadInferenceError;
    }

    ++state.totalChecked;
    state.prevRect = faceRect;
    state.havePrevRect = true;
    if (score >= m_config.antiSpoofThreshold) {
        state.timing.OnPadScore(true);
    }
    FACELOGIN_DEBUG(L"Anti-spoof frame %d: score=%.3f (pass=%d)",
                    state.totalChecked, score, state.timing.PassCount());
    return std::nullopt;
}

// Maps the loop's terminal state to the user-facing outcome. Only
// CountingComplete with complete evidence succeeds; every other stop picks
// the most specific honest failure message.
AuthPipelineResult AuthPipeline::ResolveOutcome(AuthLoopStop stop,
                                                const AuthLoopState& state) const {
    const int totalChecks = AntiSpoofCheckCount(m_config.fastUnlock);
    const int passRequired = AntiSpoofPassRequired(totalChecks);
    AuthPipelineResult result;
    const bool livenessPassed = stop == AuthLoopStop::CountingComplete &&
        state.totalChecked == totalChecks &&
        state.timing.PassCount() >= passRequired && state.bindingCount == 3;
    if (livenessPassed) {
        FACELOGIN_INFO(L"Liveness passed — tail anchor bound, authentication evidence "
                       L"complete (width=%ld px, face luma=%.0f)",
                       state.boundFaceWidth, state.boundFaceLuma);
        result.outcome = AuthOutcome::Succeeded;
        return result;
    }

    FACELOGIN_WARN(L"Authentication evidence incomplete: identity=%u/3, "
                   L"PAD=%d/%d passed (need %d)",
                   state.bindingCount, state.timing.PassCount(), state.totalChecked,
                   passRequired);
    if (stop == AuthLoopStop::PadInferenceError) {
        result.errorMessage = L"活体检测模块异常，请使用密码登录";
    } else if (stop == AuthLoopStop::IdentityRejected) {
        result.errorMessage = state.identityError;
    } else if (!state.timing.AnyFaceSeen()) {
        result.errorMessage = L"未检测到人脸";
    } else if (stop == AuthLoopStop::IdentityRetryTimedOut ||
               (state.totalChecked == 0 && state.sawUnknownFace) ||
               (state.timing.IdentityRetryPending() && state.bindingCount < 3 &&
                state.timing.PassCount() == state.totalChecked)) {
        result.errorMessage = state.bindingCount == 0
            ? L"未识别到已注册人脸，请使用密码登录"
            : L"人脸匹配持续失败，请调整位置或光照后重试";
    } else {
        result.errorMessage = L"未通过活体检测，请使用真实人脸";
    }
    return result;
}

// Failed-round progress rule (2026-10-16 09:24–09:25 post-mortem): a round
// whose knob writes moved a no-face or out-of-band scene toward the band
// lost that progress at the per-round process boundary — the next worker
// replayed the previous combo and repeated the same climb, oscillating
// forever in a genuinely dark room. When the round carries face-backed
// evidence of progress, persist the recorded write target so the next round
// resumes instead of restarting. Success rounds never enter: their ordinary
// in-band saves stay the only writers.
void AuthPipeline::PersistFailedRoundTuneProgress(const PreparedAuthFrames& prepared) const {
    const auto& cfg = prepared.tuneConfig;
    if (cfg.persistPath.empty()) return;
    const auto verdict = EvaluateFailedRoundTuneProgress(
        prepared.triggerFaceLuma, prepared.lastFaceLuma, prepared.tuneWrites, cfg);
    if (verdict == TuneProgressVerdict::NotNeeded) {
        if (prepared.tuneWrites.wrote) {
            FACELOGIN_INFO(L"Failed-round tune progress not saved: trigger luma %.0f, "
                           L"settled %.0f, last face %.0f — no face-backed gain",
                           prepared.triggerFaceLuma, prepared.tuneWrites.lastSettledLuma,
                           prepared.lastFaceLuma);
        }
        return;
    }
    if (SaveTuneKnobState(cfg.persistPath, prepared.tuneWrites.lastWritten)) {
        FACELOGIN_INFO(L"Failed-round tune progress saved (%s): trigger luma %.0f, "
                       L"settled %.0f, last face %.0f — exposure %ld (manual), gain %ld",
                       verdict == TuneProgressVerdict::SaveFaceAppeared
                           ? L"face appeared" : L"settled toward band",
                       prepared.triggerFaceLuma, prepared.tuneWrites.lastSettledLuma,
                       prepared.lastFaceLuma,
                       prepared.tuneWrites.lastWritten.hasExposure
                           ? prepared.tuneWrites.lastWritten.exposure : 0,
                       prepared.tuneWrites.lastWritten.hasGain
                           ? prepared.tuneWrites.lastWritten.gain : 0);
    } else {
        FACELOGIN_WARN(L"Failed to save failed-round tune progress");
    }
}

} // namespace facelogin
