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
        if (!PrepareFrames(prepared, exposureTiming, sensorKnobs, interrupted, grabFrame))
            return result;
        auto& tuneCfg = prepared.tuneConfig;
        auto& tuneRecovery = prepared.tuneRecovery;
        auto& darkRecoveryAttempted = prepared.darkRecoveryAttempted;

        const auto statusAt = std::chrono::steady_clock::now();
        m_callbacks.reportStatus(L"正在识别...");
        exposureTiming.statusMs = ElapsedMs(statusAt);
        exposureTiming.gateToLoopMs = ElapsedMs(prepared.gateAt);
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
        // The parent verification callback returns Retry only for "no enrolled identity
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
        const bool sensorKnobsWired = !prepared.usingAutoExposure &&
            (static_cast<bool>(sensorKnobs.exposureSet) ||
             static_cast<bool>(sensorKnobs.gainSet));

        AuthFrameStream frames(m_detector, m_config.cameraRotation, grabFrame, [&] {
            return interrupted() || m_callbacks.isClientDisconnected() ||
                timing.WindowExpired(std::chrono::steady_clock::now());
        });

        AuthFrame next = std::move(prepared.seed);            // warmup SCRFD pre-run seed, if fresh
        while (totalChecked < totalChecks) {
            if (interrupted()) {
                return result;
            }
            if (m_callbacks.isClientDisconnected()) {
                FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
                result.outcome = AuthOutcome::Cancelled;
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
                if (!frames.Grab(next)) {
                    if (interrupted()) {
                        return result;
                    }
                    continue;                       // grab failure: legacy 30 ms pace already served
                }
            }
            AuthFrame cur = std::move(next);
            next = AuthFrame{};

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
            frames.AdvanceAnchor(cur);

            // Overlap window: PAD(N) is running while the main thread
            // produces frame N+1's capture+detection. The +1 accounts for
            // this frame being counted as totalChecked+1; skip prefetch when
            // it would be the last frame. A wasted prefetch (this frame later
            // fails on an invalid PAD score) costs CPU only, never latency.
            if (totalChecked + 1 < totalChecks) {
                AuthFrame candidate;
                if (frames.Grab(candidate)) {
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

        if (const auto minInterval = frames.MinimumIntervalMs()) {
            FACELOGIN_INFO(L"Anti-spoof frame pacing: min grab interval %.1f ms over %d counted frames (guard 60 ms)",
                           *minInterval, totalChecked);
        }

        const bool livenessPassed = !failFastEmpty && !failFastAttack &&
            !livenessInferenceError && !identityRejected && !identityRetryTimedOut &&
            totalChecked == totalChecks && timing.PassCount() >= passRequired && bindingCount == 3;
        if (livenessPassed) {
            FACELOGIN_INFO(L"Liveness passed — tail anchor bound, authentication evidence "
                           L"complete (width=%ld px, face luma=%.0f)",
                           boundFaceWidth, boundFaceLuma);
            result.outcome = AuthOutcome::Succeeded;
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
