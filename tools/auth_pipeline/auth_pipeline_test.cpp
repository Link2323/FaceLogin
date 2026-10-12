// Exercise AuthPipeline::Run itself, including its async PAD/detection and
// binding Retry branches. Only inference is replaced; no production test hook
// is needed and no face image, model or Windows credential is consumed.
#include "auth_pipeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <utility>

namespace {
std::atomic<float> g_padScore{1.0f};
int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}
} // namespace

// These definitions replace onnx_models.cpp ONLY in this development target.
namespace facelogin {
OnnxDetector::~OnnxDetector() = default;
OnnxRecognizer::~OnnxRecognizer() = default;
MiniFasEvaluator::~MiniFasEvaluator() = default;
OnnxAntiSpoof::~OnnxAntiSpoof() = default;

bool OnnxAntiSpoof::Initialize(const std::wstring&, const std::wstring&) {
    m_initialized = true;
    return true;
}

std::optional<OnnxDetector::Detection> OnnxDetector::DetectLargestFace(const FrameImage&) {
    return Detection{20, 20, 119, 119, 1.0f,
                     {60, 45, 100, 45, 80, 80, 60, 100, 100, 100}};
}

std::vector<float> OnnxRecognizer::ComputeEmbedding(const FrameImage&, const float[10],
                                                   float* preNorm) {
    if (preNorm) *preNorm = 10.0f;
    std::vector<float> embedding(128, 0.0f);
    embedding.front() = 1.0f;
    return embedding;
}

float OnnxAntiSpoof::Predict(const FrameImage&, const FaceRect&) {
    return g_padScore.load();
}
} // namespace facelogin

namespace {
using namespace facelogin;
using Clock = std::chrono::steady_clock;

AuthPipelineResult Run(bool fastUnlock,
                       const decltype(AuthPipelineCallbacks::verifyBinding)& binding,
                       const std::function<bool()>& cancelled = [] { return false; }) {
    OnnxDetector detector;
    OnnxRecognizer recognizer;
    OnnxAntiSpoof pad;
    Check(pad.Initialize({}, {}), "inference double initializes");
    FrameImage fixture(160, 160);
    for (size_t i = 0; i < fixture.size(); ++i) fixture[i] = RgbPixel(80, 80, 80);
    unsigned long long sequence = 0;
    AuthPipelineCallbacks callbacks;
    callbacks.grabFrame = [&](FrameImage& frame, unsigned long long& seq) {
        frame = fixture;
        seq = ++sequence;
        return true;
    };
    callbacks.isCancelled = cancelled;
    callbacks.isClientDisconnected = [] { return false; };
    callbacks.reportStatus = [](const std::wstring&) {};
    callbacks.verifyBinding = binding;
    AuthPipelineConfig config;
    config.fastUnlock = fastUnlock;
    return AuthPipeline(detector, recognizer, pad, config, std::move(callbacks)).Run();
}

void TestPersistentMissAfterPartialEvidence(bool fastUnlock, unsigned int accepted) {
    g_padScore = 1.0f;
    unsigned int accepts = 0, retries = 0;
    const auto start = Clock::now();
    const auto result = Run(fastUnlock,
        [&](const std::vector<float>&, unsigned int index, float, float, float) {
            Check(index == accepts, "Retry preserves the pending binding index");
            if (accepts < accepted) {
                ++accepts;
                return BindingDecision{BindingDecisionKind::Accept, {}};
            }
            ++retries;
            return BindingDecision{BindingDecisionKind::Retry, {}};
        });
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    Check(accepts == accepted && retries > 1, "partial evidence followed by repeated identity misses");
    Check(!result.succeeded && !result.cancelled && !result.timedOut,
          "insufficient identity evidence never authenticates");
    Check(elapsed >= 1.9 && elapsed < 3.5,
          "persistent identity misses fail near 2s instead of waiting 8s");
    Check(result.errorMessage == L"人脸匹配持续失败，请调整位置或光照后重试",
          "partial identity failure is not reported as a PAD rejection");
    std::printf("Partial evidence (%s, %u bindings): %.3f s\n",
                fastUnlock ? "fast" : "default", accepted, elapsed);
}

void TestTransientMissRecovers(bool fastUnlock) {
    g_padScore = 1.0f;
    unsigned int accepts = 0, retries = 0;
    const auto result = Run(fastUnlock,
        [&](const std::vector<float>&, unsigned int index, float, float, float) {
            Check(index == accepts, "recovery keeps binding indices ordered");
            if (index == 1 && retries++ < 3) {
                return BindingDecision{BindingDecisionKind::Retry, {}};
            }
            ++accepts;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        });
    Check(result.succeeded && accepts == 3,
          "transient identity misses recover only after all three bindings");
}

void TestPadFailureRemainsPadFailure() {
    g_padScore = 0.0f;
    const auto result = Run(true,
        [](const std::vector<float>&, unsigned int, float, float, float) {
            return BindingDecision{BindingDecisionKind::Accept, {}};
        });
    Check(!result.succeeded && result.errorMessage == L"未通过活体检测，请使用真实人脸",
          "matching identity cannot bypass failing PAD scores");
}

void TestRecoveryResetsClockInPipeline() {
    g_padScore = 1.0f;
    unsigned int accepts = 0, retryIndex = 0;
    Clock::time_point retryStart{};
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int index, float, float, float) {
            if (index != 0) {
                if (retryIndex != index) {
                    retryIndex = index;
                    retryStart = Clock::now();
                }
                if (Clock::now() - retryStart < std::chrono::milliseconds(1200)) {
                    return BindingDecision{BindingDecisionKind::Retry, {}};
                }
            }
            ++accepts;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        });
    Check(result.succeeded && accepts == 3,
          "accepted binding resets the clock even when total retry time exceeds 2s");
}

void TestIdentityRejectRemainsTerminal() {
    g_padScore = 1.0f;
    const auto result = Run(true,
        [](const std::vector<float>&, unsigned int index, float, float, float) {
            return index == 0
                ? BindingDecision{BindingDecisionKind::Accept, {}}
                : BindingDecision{BindingDecisionKind::Reject, L"活体验证期间人脸不匹配，请重试"};
        });
    Check(!result.succeeded && result.errorMessage == L"活体验证期间人脸不匹配，请重试",
          "explicit identity rejection is not converted into a retry");
}

void TestInvalidPadRemainsTerminal() {
    g_padScore = -1.0f;
    const auto result = Run(true,
        [](const std::vector<float>&, unsigned int, float, float, float) {
            return BindingDecision{BindingDecisionKind::Accept, {}};
        });
    Check(!result.succeeded && result.errorMessage == L"活体检测模块异常，请使用密码登录",
          "invalid PAD inference still fails closed with its own error");
}

void TestCancelDuringIdentityRetry() {
    g_padScore = 1.0f;
    const auto start = Clock::now();
    const auto result = Run(true,
        [](const std::vector<float>&, unsigned int index, float, float, float) {
            return BindingDecision{index == 0 ? BindingDecisionKind::Accept
                                             : BindingDecisionKind::Retry, {}};
        }, [&] { return Clock::now() - start >= std::chrono::milliseconds(300); });
    Check(result.cancelled && !result.succeeded && result.errorMessage.empty(),
          "cancellation wins over identity failure and joins inference work");
}
} // namespace

int main() {
    TestPersistentMissAfterPartialEvidence(true, 1);
    TestPersistentMissAfterPartialEvidence(true, 2);
    TestPersistentMissAfterPartialEvidence(false, 2);
    TestTransientMissRecovers(true);
    TestTransientMissRecovers(false);
    TestRecoveryResetsClockInPipeline();
    TestPadFailureRemainsPadFailure();
    TestIdentityRejectRemainsTerminal();
    TestInvalidPadRemainsTerminal();
    TestCancelDuringIdentityRetry();
    if (g_failures == 0) std::puts("AuthPipelineTest: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
