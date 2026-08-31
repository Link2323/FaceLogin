#include "auth_pipeline.h"

#include "../common/logger.h"
#include "exposure_warmup.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>

namespace facelogin {

// Mean luma over the face bbox — the capture condition that dominates
// identity-match distance (dark chips shift embeddings; see
// config_util.h low_light_enhance). Diagnostic only, computed once per
// outcome-relevant frame.
static float FaceRegionLuma(const FrameImage& frame, const FaceRect& r) {
    const long y0 = std::max<long>(0, r.top());
    const long y1 = std::min<long>(frame.nr() - 1, r.bottom());
    const long x0 = std::max<long>(0, r.left());
    const long x1 = std::min<long>(frame.nc() - 1, r.right());
    double sum = 0.0;
    long n = 0;
    for (long y = y0; y <= y1; ++y) {
        for (long x = x0; x <= x1; ++x) {
            const auto& p = frame(y, x);
            sum += 0.299 * p.red + 0.587 * p.green + 0.114 * p.blue;
            ++n;
        }
    }
    return n > 0 ? static_cast<float>(sum / n) : -1.0f;
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

    try {
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
        // free. When the gate opens, the freshest frame + detection seeds the
        // prefetch slot, removing the first serial grab+detect from the
        // critical path. The seed is a real, distinct, rotated camera frame
        // and the first counted frame has no pacing anchor yet, so no
        // invariant is touched. Seeds older than 400 ms (slow dark-room
        // convergence) are discarded and the loop grabs fresh exactly like
        // the legacy behavior.
        constexpr double kSeedMaxAgeMs = 400.0;
        Prefetch seed;
        std::future<std::optional<OnnxDetector::Detection>> seedFuture;
        FrameImage seedFrame;
        unsigned long long seedSeq = 0;
        std::chrono::steady_clock::time_point seedWall{};

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
            if (m_callbacks.isCancelled()) {
                result.cancelled = true;
                return result;
            }
            FrameImage warmFrame;
            unsigned long long seq = 0;
            if (!grabFrame(warmFrame, seq) || (haveLastSeq && seq == lastSeq)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            haveLastSeq = true;
            lastSeq = seq;
            if (seedFuture.valid() &&
                seedFuture.wait_for(std::chrono::seconds(0))
                    == std::future_status::ready) {
                auto seedDet = seedFuture.get();
                seed = Prefetch{ true, std::move(seedFrame), seedSeq,
                                 std::move(seedDet), seedWall };
            }
            if (!seedFuture.valid()) {
                seedFrame = warmFrame;               // luma already sampled; copy for detection
                RotateFrame(seedFrame, m_config.cameraRotation);
                seedSeq = seq;
                seedWall = std::chrono::steady_clock::now();
                seedFuture = std::async(std::launch::async, [this, &seedFrame] {
                    return m_detector.DetectLargestFace(seedFrame);
                });
            }
            if (warmup.Feed(MeanLuma(warmFrame))) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        FACELOGIN_INFO(L"Exposure warmup: %d samples, %.0f ms%s",
                       warmup.Samples(),
                       std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - warmupStart).count(),
                       warmup.Settled() ? L" — stable" : L" — cap reached, proceeding");

        if (seedFuture.valid()) {
            // Collecting the outstanding detection costs at most what the
            // loop's first serial detect would have cost — never a regression.
            auto seedDet = seedFuture.get();
            seed = Prefetch{ true, std::move(seedFrame), seedSeq,
                             std::move(seedDet), seedWall };
        }
        if (seed.valid && std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - seed.grabWall).count()
                              > kSeedMaxAgeMs) {
            seed = Prefetch{};                      // stale seed: grab fresh like the legacy loop
        }

        m_callbacks.reportStatus(L"正在识别...");
        const auto startTime = std::chrono::steady_clock::now();
        // No per-round echo of the PAD threshold/check counts here: the
        // active config (threshold included) is logged once per load by
        // LoadConfig, and "need N" appears in the failure line.
        const int totalChecks = AntiSpoofCheckCount(m_config.antiSpoofThreshold);
        const int passRequired = AntiSpoofPassRequired(totalChecks);

        const auto livenessStart = std::chrono::steady_clock::now();
        LivenessTiming timing{livenessStart};
        int totalChecked = 0;
        unsigned int bindingCount = 0;
        bool havePrevRect = false;
        FaceRect prevRect{};
        bool livenessInferenceError = false;
        bool identityRejected = false;
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

        // Returns false on cancel or grab failure; the caller re-checks
        // isCancelled() to distinguish. Failed grabs keep the legacy 30 ms
        // retry pace outside this helper's loop.
        const auto grabWithPacing = [&](Prefetch& out) {
            for (;;) {
                if (m_callbacks.isCancelled()) return false;
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
            if (m_callbacks.isCancelled()) {
                result.cancelled = true;
                return result;
            }
            if (m_callbacks.isClientDisconnected()) {
                FACELOGIN_INFO(L"Client disconnected during anti-spoof — aborting");
                result.cancelled = true;
                return result;
            }

            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count()
                >= m_config.authTimeoutSeconds) {
                FACELOGIN_INFO(L"Authentication timed out");
                result.timedOut = true;
                return result;
            }
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
                    if (m_callbacks.isCancelled()) {
                        result.cancelled = true;
                        return result;
                    }
                    continue;                       // grab failure: legacy 30 ms pace already served
                }
            }
            Prefetch cur = std::move(next);
            next = Prefetch{};

            if (!cur.det) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
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

            const FaceRect faceRect(static_cast<long>(cur.det->x1),
                                    static_cast<long>(cur.det->y1),
                                    static_cast<long>(cur.det->x2),
                                    static_cast<long>(cur.det->y2));
            const bool isAnchor = bindingCount == 0 || totalChecked == totalChecks - 1;
            const bool isConsensusFrame = totalChecked == 2;

            auto scoreFuture = std::async(std::launch::async, [&cur, &faceRect, this] {
                return m_antiSpoof.Predict(cur.frame, faceRect);
            });

            if (isAnchor || isConsensusFrame) {
                auto embedding = m_recognizer.ComputeEmbedding(cur.frame, cur.det->kps);
                if (embedding.empty()) {
                    scoreFuture.get();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }

                const BindingDecision decision =
                    m_callbacks.verifyBinding(embedding, bindingCount);
                if (decision.kind == BindingDecisionKind::Retry) {
                    scoreFuture.get();
                    if (!haveRetryConditions) {
                        retryFaceWidth = faceRect.width();
                        retryFaceLuma = FaceRegionLuma(cur.frame, faceRect);
                        haveRetryConditions = true;
                    }
                    sawUnknownFace = true;
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
                } else if (m_callbacks.isCancelled()) {
                    // The helper only fails after a cancel check or a grab
                    // failure; cancel must abort before frame N is counted.
                    scoreFuture.get();
                    result.cancelled = true;
                    return result;
                }
                // Grab failure: leave the slot empty — frame N still counts
                // below, and the next iteration grabs fresh at loop pace.
            }

            const float score = scoreFuture.get();
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
            !livenessInferenceError && !identityRejected &&
            totalChecked == totalChecks && timing.PassCount() >= passRequired && bindingCount == 3;
        if (livenessPassed) {
            FACELOGIN_INFO(L"Liveness passed — tail anchor bound, authentication evidence "
                           L"complete (width=%ld px, face luma=%.0f)",
                           boundFaceWidth, boundFaceLuma);
            result.succeeded = true;
            return result;
        }

        FACELOGIN_WARN(L"Anti-spoof check failed: %d/%d passed (need %d)",
                       timing.PassCount(), totalChecked, passRequired);
        if (livenessInferenceError) {
            result.errorMessage = L"活体检测模块异常，请使用密码登录";
        } else if (identityRejected) {
            result.errorMessage = identityError;
        } else if (!timing.AnyFaceSeen()) {
            result.errorMessage = L"未检测到人脸";
        } else if (totalChecked == 0 && sawUnknownFace) {
            result.errorMessage = L"未识别到已注册人脸，请使用密码登录";
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
