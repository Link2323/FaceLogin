// Exercise AuthPipeline::Run itself, including its async PAD/detection and
// binding Retry branches. Only inference is replaced; no production test hook
// is needed and no face image, model or Windows credential is consumed.
#include "auth_pipeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

namespace {
std::atomic<float> g_padScore{1.0f};
std::atomic<int> g_minDetectLuma{0};
std::atomic<bool> g_smallBacklitFace{false};
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

std::optional<OnnxDetector::Detection> OnnxDetector::DetectLargestFace(const FrameImage& frame) {
    if (frame.size() == 0 ||
        frame(frame.nr() / 2, frame.nc() / 2).red < g_minDetectLuma.load()) return std::nullopt;
    if (g_smallBacklitFace.load())
        return Detection{65, 65, 94, 94, 1.0f,
                         {72, 72, 87, 72, 80, 80, 72, 88, 87, 88}};
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
                       const std::function<bool()>& cancelled = [] { return false; },
                       const std::function<bool()>& frozen = [] { return false; },
                       int timeoutSeconds = ipc::AUTH_TIMEOUT_SECONDS,
                       const std::function<bool()>& disconnected = [] { return false; },
                       const std::function<void(AuthPipelineCallbacks&, AuthPipelineConfig&)>& setup = {}) {
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
        if (!frozen()) ++sequence;
        seq = sequence;
        return true;
    };
    callbacks.isCancelled = cancelled;
    callbacks.isClientDisconnected = disconnected;
    callbacks.reportStatus = [](const std::wstring&) {};
    callbacks.verifyBinding = binding;
    AuthPipelineConfig config;
    config.fastUnlock = fastUnlock;
    config.authTimeoutSeconds = timeoutSeconds;
    if (setup) setup(callbacks, config);
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

void TestFrozenCameraHonorsTimeout() {
    g_padScore = 1.0f;
    unsigned int accepts = 0;
    const auto start = Clock::now();
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            ++accepts;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }, [&] { return Clock::now() - start >= std::chrono::milliseconds(1500); },
        [&] { return accepts != 0; }, 1);
    Check(accepts == 1, "frozen camera cannot supply more identity evidence");
    Check(result.timedOut && !result.cancelled && !result.succeeded,
          "waiting for a new camera sequence honors the authentication deadline");
    std::printf("Frozen camera: %.3f s, timedOut=%d, cancelled=%d\n",
                std::chrono::duration<double>(Clock::now() - start).count(),
                result.timedOut, result.cancelled);
}

void TestFrozenCameraHonorsDisconnect() {
    g_padScore = 1.0f;
    unsigned int accepts = 0;
    const auto start = Clock::now();
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            ++accepts;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }, [&] { return Clock::now() - start >= std::chrono::milliseconds(1000); },
        [&] { return accepts != 0; }, 15,
        [&] { return Clock::now() - start >= std::chrono::milliseconds(200); });
    Check(result.cancelled && !result.succeeded && accepts == 1,
          "client disconnect aborts a frozen camera without counting stale evidence");
    Check(Clock::now() - start < std::chrono::milliseconds(800),
          "frozen camera observes disconnect before watchdog cancellation");
}

void TestPreparationConsumesAuthBudget() {
    unsigned int bindings = 0;
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            ++bindings;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }, [] { return false; }, [] { return false; }, 1, [] { return false; },
        [](AuthPipelineCallbacks& callbacks, AuthPipelineConfig&) {
            callbacks.sensorKnobs.gainRange = [](long&, long&, long&) { return false; };
            callbacks.sensorKnobs.exposureGet = [](long& value) { value = -5; return true; };
            callbacks.sensorKnobs.exposureSetAuto = [](long) { return true; };
            callbacks.sensorKnobs.exposureIsManual = [](bool& manual) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                manual = false;
                return true;
            };
        });
    Check(result.timedOut && !result.succeeded && bindings == 0,
          "exposure preparation consumes the authentication budget before any binding");
}

void TestExpiredHostDeadlineNeverBinds() {
    unsigned int bindings = 0;
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            ++bindings;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }, [] { return false; }, [] { return false; }, ipc::AUTH_TIMEOUT_SECONDS,
        [] { return false; }, [](AuthPipelineCallbacks&, AuthPipelineConfig& config) {
            config.authDeadline = ipc::MakeAuthDeadline(0);
        });
    Check(result.timedOut && !result.succeeded && bindings == 0,
          "an expired host deadline cannot restart a fresh pipeline budget");
}

void TestNoFaceDarkRecovery(bool lateEntry = false, bool padPass = true,
                           int timeoutSeconds = ipc::AUTH_TIMEOUT_SECONDS,
                           bool cancelAfterWrite = false, bool backlit = false) {
    // Detection stays unavailable until the real orchestration brightens the
    // sensor. No pre-existing face box is available to trigger face tuning.
    g_minDetectLuma = 50;
    g_smallBacklitFace = backlit;
    g_padScore = padPass ? 1.0f : 0.0f;
    long gain = 50;
    unsigned int writes = 0, bindings = 0;
    unsigned long long sequence = 0;
    const auto start = Clock::now();
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            ++bindings;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }, [&] { return cancelAfterWrite && writes > 0; }, [] { return false; }, timeoutSeconds,
        [] { return false; }, [&](AuthPipelineCallbacks& callbacks, AuthPipelineConfig&) {
            callbacks.grabFrame = [&](FrameImage& frame, unsigned long long& seq) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                frame = FrameImage(160, 160);
                const auto luma = static_cast<unsigned char>(lateEntry && sequence < 4
                    ? 40 : 20 + (gain - 50) * 8);
                for (size_t i = 0; i < frame.size(); ++i) frame[i] = RgbPixel(luma, luma, luma);
                if (backlit) {
                    for (size_t i = 0; i < frame.size(); ++i) frame[i] = RgbPixel(180, 180, 180);
                    for (long y = 65; y < 95; ++y)
                        for (long x = 65; x < 95; ++x) frame(y, x) = RgbPixel(luma, luma, luma);
                }
                seq = ++sequence;
                return true;
            };
            auto& knobs = callbacks.sensorKnobs;
            knobs.exposureGet = [](long& value) { value = -5; return true; };
            knobs.exposureIsManual = [](bool& manual) { manual = true; return true; };
            knobs.gainRange = [](long& lo, long& hi, long& step) {
                lo = 0; hi = 63; step = 1; return true;
            };
            knobs.gainGet = [&](long& value) { value = gain; return true; };
            knobs.gainSet = [&](long value) { gain = value; ++writes; return true; };
        });
    g_minDetectLuma = 0;
    g_smallBacklitFace = false;
    if (cancelAfterWrite) {
        Check(result.cancelled && !result.succeeded && writes == 1 && bindings == 0,
              "cancellation during dark recovery stops before any identity evidence");
    } else if (timeoutSeconds == 1) {
        Check(result.timedOut && !result.succeeded && bindings == 0 &&
              Clock::now() - start < std::chrono::milliseconds(1500),
              "dark recovery shares the original deadline and cannot extend authentication");
    } else if (!padPass) {
        Check(writes > 0 && !result.succeeded,
              "successful dark recovery cannot bypass failed PAD evidence");
    } else {
        Check(writes > 0 && result.succeeded && bindings == 3,
              "no-face dark scene brightens before detection, then requires all identity bindings");
    }
    std::printf("No-face dark recovery (late=%d, PAD=%d, timeout=%d, cancel=%d, backlit=%d): "
                "%.3f s, writes=%u, bindings=%u, succeeded=%d\n",
                lateEntry, padPass, timeoutSeconds, cancelAfterWrite, backlit,
                std::chrono::duration<double>(Clock::now() - start).count(),
                writes, bindings, result.succeeded);
}
} // namespace

int main(int argc, char** argv) {
    TestNoFaceDarkRecovery();
    if (argc == 2 && std::strcmp(argv[1], "--dark-recovery") == 0)
        return g_failures == 0 ? 0 : 1;
    TestNoFaceDarkRecovery(false, true, ipc::AUTH_TIMEOUT_SECONDS, false, true);
    TestNoFaceDarkRecovery(true);
    TestNoFaceDarkRecovery(false, false);
    TestNoFaceDarkRecovery(false, true, 1);
    TestNoFaceDarkRecovery(false, true, ipc::AUTH_TIMEOUT_SECONDS, true);
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
    TestFrozenCameraHonorsTimeout();
    TestFrozenCameraHonorsDisconnect();
    TestPreparationConsumesAuthBudget();
    TestExpiredHostDeadlineNeverBinds();
    if (g_failures == 0) std::puts("AuthPipelineTest: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
