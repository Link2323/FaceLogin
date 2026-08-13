#include "auth_pipeline.h"

#include "../common/logger.h"

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
    if (m_config.livenessMethod != LivenessMethod::AntiSpoof ||
        !m_antiSpoof.IsInitialized()) {
        result.errorMessage = L"活体检测模块不可用，请使用密码登录";
        FACELOGIN_ERROR(L"AuthPipeline refused to run without anti-spoof");
        return result;
    }

    try {
        const auto pipelineStart = std::chrono::steady_clock::now();
        bool firstFrameLogged = false;
        const auto grabFrame = [this, pipelineStart, &firstFrameLogged](FrameImage& frame) {
            const bool grabbed = m_callbacks.grabFrame(frame);
            if (grabbed && !firstFrameLogged) {
                firstFrameLogged = true;
                FACELOGIN_INFO(L"Auth pipeline first valid frame %.1f ms after camera ready",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - pipelineStart).count());
            }
            return grabbed;
        };

        // Let auto-exposure settle. Keep this timing and count identical to
        // the old in-process path; moving the loop to a worker must not change
        // PAD timing or the calibrated recognition behavior.
        FrameImage frame;
        for (int i = 0; i < 3; ++i) {
            if (m_callbacks.isCancelled()) {
                result.cancelled = true;
                return result;
            }
            if (grabFrame(frame)) {
                RotateFrame(frame, m_config.cameraRotation);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        m_callbacks.reportStatus(L"正在识别...");
        const auto startTime = std::chrono::steady_clock::now();
        const int totalChecks = AntiSpoofCheckCount(m_config.antiSpoofThreshold);
        const int passRequired = AntiSpoofPassRequired(totalChecks);
        FACELOGIN_INFO(L"Anti-spoof: threshold=%.3f → %d checks, %d required",
                       m_config.antiSpoofThreshold, totalChecks, passRequired);

        const auto livenessStart = std::chrono::steady_clock::now();
        int passCount = 0;
        int totalChecked = 0;
        unsigned int bindingCount = 0;
        bool havePrevRect = false;
        FaceRect prevRect;
        bool anyFaceSeen = false;
        bool livenessInferenceError = false;
        bool identityRejected = false;
        bool failFastEmpty = false;
        bool failFastAttack = false;
        std::wstring identityError;
        auto noFaceStart = livenessStart;
        auto noPassStart = livenessStart;

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
            if (std::chrono::duration_cast<std::chrono::seconds>(now - livenessStart).count() >= 8) {
                break;
            }
            if (!anyFaceSeen &&
                std::chrono::duration<double>(now - noFaceStart).count() >= 2.5) {
                FACELOGIN_INFO(L"No face detected within 2.5s — failing fast (empty scene)");
                failFastEmpty = true;
                break;
            }

            FrameImage asFrame;
            if (!grabFrame(asFrame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            RotateFrame(asFrame, m_config.cameraRotation);

            auto asDet = m_detector.DetectLargestFace(asFrame);
            if (!asDet) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }
            anyFaceSeen = true;

            if (passCount == 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - noPassStart).count() >= 2.0) {
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
                ++passCount;
                noPassStart = std::chrono::steady_clock::now();
            }
            FACELOGIN_DEBUG(L"Anti-spoof frame %d: score=%.3f (pass=%d)",
                            totalChecked, score, passCount);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }

        const bool livenessPassed = !failFastEmpty && !failFastAttack &&
            !livenessInferenceError && !identityRejected &&
            totalChecked == totalChecks && passCount >= passRequired && bindingCount == 3;
        if (livenessPassed) {
            FACELOGIN_INFO(L"Liveness passed — tail anchor bound, authentication evidence complete");
            result.succeeded = true;
            return result;
        }

        FACELOGIN_WARN(L"Anti-spoof check failed: %d/%d passed (need %d)",
                       passCount, totalChecked, passRequired);
        if (livenessInferenceError) {
            result.errorMessage = L"活体检测模块异常，请使用密码登录";
        } else if (identityRejected) {
            result.errorMessage = identityError;
        } else if (!anyFaceSeen) {
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
