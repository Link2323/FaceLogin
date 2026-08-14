#include "auth_pipeline.h"

#include "../common/logger.h"
#include "exposure_warmup.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>

namespace facelogin {

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

        m_callbacks.reportStatus(L"正在识别...");
        const auto startTime = std::chrono::steady_clock::now();
        const int totalChecks = AntiSpoofCheckCount(m_config.antiSpoofThreshold);
        const int passRequired = AntiSpoofPassRequired(totalChecks);
        FACELOGIN_INFO(L"Anti-spoof: threshold=%.3f → %d checks, %d required",
                       m_config.antiSpoofThreshold, totalChecks, passRequired);

        const auto livenessStart = std::chrono::steady_clock::now();
        LivenessTiming timing{livenessStart};
        int totalChecked = 0;
        unsigned int bindingCount = 0;
        bool havePrevRect = false;
        FaceRect prevRect;
        bool livenessInferenceError = false;
        bool identityRejected = false;
        bool failFastEmpty = false;
        bool failFastAttack = false;
        std::wstring identityError;

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

            FrameImage asFrame;
            unsigned long long asSeq = 0;
            if (!grabFrame(asFrame, asSeq)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            RotateFrame(asFrame, m_config.cameraRotation);

            auto asDet = m_detector.DetectLargestFace(asFrame);
            if (!asDet) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            const auto faceNow = std::chrono::steady_clock::now();
            timing.OnFaceDetected(faceNow);

            if (timing.ShouldFailPersistentAttack(faceNow)) {
                FACELOGIN_INFO(L"PAD persistently below threshold for 2s — failing fast (likely attack)");
                failFastAttack = true;
                break;
            }

            const FaceRect faceRect(static_cast<long>(asDet->x1),
                                    static_cast<long>(asDet->y1),
                                    static_cast<long>(asDet->x2),
                                    static_cast<long>(asDet->y2));
            const bool isAnchor = bindingCount == 0 || totalChecked == totalChecks - 1;
            const bool isConsensusFrame = totalChecked == 2;

            auto scoreFuture = std::async(std::launch::async, [&] {
                return m_antiSpoof.Predict(asFrame, faceRect);
            });

            if (isAnchor || isConsensusFrame) {
                auto embedding = m_recognizer.ComputeEmbedding(asFrame, asDet->kps);
                if (embedding.empty()) {
                    scoreFuture.get();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    continue;
                }

                const BindingDecision decision =
                    m_callbacks.verifyBinding(embedding, bindingCount);
                if (decision.kind == BindingDecisionKind::Retry) {
                    scoreFuture.get();
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
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }

        const bool livenessPassed = !failFastEmpty && !failFastAttack &&
            !livenessInferenceError && !identityRejected &&
            totalChecked == totalChecks && timing.PassCount() >= passRequired && bindingCount == 3;
        if (livenessPassed) {
            FACELOGIN_INFO(L"Liveness passed — tail anchor bound, authentication evidence complete");
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
