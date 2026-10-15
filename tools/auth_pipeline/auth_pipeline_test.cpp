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
std::atomic<unsigned int> g_detectCalls{0};
std::atomic<int> g_detectionDelayMs{0};
std::atomic<unsigned int> g_activeDetections{0};
int g_lastEmbeddingMarker = 0;
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
    ++g_activeDetections;
    struct DetectionFinished {
        ~DetectionFinished() { --g_activeDetections; }
    } finished;
    ++g_detectCalls;
    if (const int delay = g_detectionDelayMs.load(); delay > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    if (frame.size() == 0 ||
        frame(frame.nr() / 2, frame.nc() / 2).red < g_minDetectLuma.load()) return std::nullopt;
    if (g_smallBacklitFace.load())
        return Detection{65, 65, 94, 94, 1.0f,
                         {72, 72, 87, 72, 80, 80, 72, 88, 87, 88}};
    return Detection{20, 20, 119, 119, 1.0f,
                     {60, 45, 100, 45, 80, 80, 60, 100, 100, 100}};
}

std::vector<float> OnnxRecognizer::ComputeEmbedding(const FrameImage& frame, const float[10],
                                                   float* preNorm) {
    g_lastEmbeddingMarker = frame(0, 0).red;
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

void TestPacingSkipsStaleFramesAndRetainsRetryAnchor() {
    g_padScore = 1.0f;
    // Warmup: 1,2. After counting 2, duplicate 2 and backward 1 must be
    // discarded. Identity retries 3 once; the same uncounted 3 remains usable.
    const std::vector<unsigned long long> sequences{1, 2, 2, 1, 3, 3, 4};
    size_t grabs = 0;
    std::vector<int> bindings;
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            bindings.push_back(g_lastEmbeddingMarker);
            return BindingDecision{bindings.size() == 2 ? BindingDecisionKind::Retry
                                                       : BindingDecisionKind::Accept, {}};
        }, [] { return false; }, [] { return false; }, ipc::AUTH_TIMEOUT_SECONDS,
        [] { return false; }, [&](AuthPipelineCallbacks& callbacks, AuthPipelineConfig&) {
            callbacks.grabFrame = [&](FrameImage& frame, unsigned long long& seq) {
                seq = grabs < sequences.size() ? sequences[grabs] : grabs;
                ++grabs;
                frame = FrameImage(160, 160);
                for (size_t i = 0; i < frame.size(); ++i) frame[i] = RgbPixel(80, 80, 80);
                frame(0, 0) = RgbPixel(static_cast<unsigned char>(seq), 80, 80);
                return true;
            };
        });
    Check(result.succeeded && bindings == std::vector<int>({2, 3, 3, 4}),
          "counted-frame dedup rejects stale/backward sequences but Retry does not advance the anchor");
    Check(grabs == sequences.size(), "frame pacing consumes exactly the expected fresh/retry frames");
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
enum class TuneRecoveryScene { Stable, PadFailure, IdentityReject, Cancel, Disconnect, Drift, Frozen };

void TestInterruptedTuneRecovery(TuneRecoveryScene scene = TuneRecoveryScene::Stable) {
    wchar_t dir[MAX_PATH]{}, path[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, dir) && GetTempFileNameW(dir, L"ftr", 0, path),
          "create interrupted-tune checkpoint");
    if (!*path) return;
    struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{path};
    long exposure = -9;
    bool hideNext = false;
    bool lost = false;
    unsigned int afterLoss = 0;
    std::vector<long> writes;
    SensorKnobs knobs;
    knobs.exposureRange = [](long& lo, long& hi, long& step) {
        lo = -11; hi = -1; step = 1; return true;
    };
    knobs.exposureGet = [&](long& value) { value = exposure; return true; };
    knobs.exposureSet = [&](long value) {
        exposure = value; writes.push_back(value); hideNext = value == -8; return true;
    };
    knobs.exposureIsManual = [](bool& manual) { manual = true; return true; };
    knobs.gainRange = [](long& lo, long& hi, long& step) {
        lo = 0; hi = 63; step = 1; return true;
    };
    knobs.gainGet = [](long& value) { value = 63; return true; };
    knobs.gainSet = [](long) { return true; };
    Check(SaveTuneKnobState(path, knobs), "seed interrupted-tune checkpoint");
    g_minDetectLuma = 1;
    g_padScore = scene == TuneRecoveryScene::PadFailure ? 0.0f : 1.0f;
    for (int round = 0; round < (scene == TuneRecoveryScene::Stable ? 2 : 1); ++round) {
        unsigned long long sequence = 0;
        unsigned int bindings = 0;
        const size_t startWrites = writes.size();
        const auto result = Run(true,
            [&](const std::vector<float>&, unsigned int, float, float, float) {
                ++bindings;
                return BindingDecision{scene == TuneRecoveryScene::IdentityReject
                    ? BindingDecisionKind::Reject : BindingDecisionKind::Accept, {}};
            }, [&] { return scene == TuneRecoveryScene::Cancel && afterLoss >= 3; },
            [] { return false; }, scene == TuneRecoveryScene::Frozen ? 1 : ipc::AUTH_TIMEOUT_SECONDS,
            [&] { return scene == TuneRecoveryScene::Disconnect && afterLoss >= 3; },
            [&](AuthPipelineCallbacks& callbacks, AuthPipelineConfig& config) {
                config.tuneStatePath = path;
                callbacks.sensorKnobs = knobs;
                callbacks.grabFrame = [&](FrameImage& frame, unsigned long long& seq) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    frame = FrameImage(160, 160);
                    if (lost) ++afterLoss;
                    const unsigned char luma = exposure == -9 ? 22
                        : scene == TuneRecoveryScene::Drift && lost ? 60 + afterLoss % 30 : 80;
                    for (size_t i = 0; i < frame.size(); ++i) frame[i] = RgbPixel(luma, luma, luma);
                    if (hideNext) {
                        frame(80, 80) = RgbPixel(0, 0, 0); hideNext = false; lost = true;
                    }
                    if (scene != TuneRecoveryScene::Frozen || afterLoss < 2) ++sequence;
                    seq = sequence; return true;
                };
            });
        TuneKnobState saved;
        const bool stable = scene == TuneRecoveryScene::Stable || scene == TuneRecoveryScene::PadFailure ||
            scene == TuneRecoveryScene::IdentityReject;
        Check(LoadTuneKnobState(path, saved) && saved.exposure == (stable ? -8 : -9),
              "only independently settled recovery replaces stale checkpoint");
        if (scene == TuneRecoveryScene::Stable)
            Check(result.succeeded && bindings == 3, "recovered tune retains all identity/PAD evidence");
        else if (scene == TuneRecoveryScene::PadFailure || scene == TuneRecoveryScene::IdentityReject)
            Check(!result.succeeded && bindings > 0, "settled exposure cannot bypass identity or PAD rejection");
        else if (scene == TuneRecoveryScene::Cancel || scene == TuneRecoveryScene::Disconnect)
            Check(result.cancelled && !result.succeeded && bindings == 0,
                  "resumed settling aborts before binding on cancellation or disconnect");
        else if (scene == TuneRecoveryScene::Frozen)
            Check(result.timedOut && !result.succeeded, "resumed settling does not restart the shared deadline");
        if (round == 1)
            Check(writes.size() == startWrites, "unchanged scene retry does not undo recovered exposure");
    }
    g_minDetectLuma = 0;
    g_padScore = 1.0f;
    std::printf("Interrupted tune recovery scene=%d: checks complete\n", static_cast<int>(scene));
}

void TestTuneRecoveries() {
    for (auto scene : {TuneRecoveryScene::Stable, TuneRecoveryScene::PadFailure,
                       TuneRecoveryScene::IdentityReject, TuneRecoveryScene::Cancel,
                       TuneRecoveryScene::Disconnect, TuneRecoveryScene::Drift,
                       TuneRecoveryScene::Frozen}) TestInterruptedTuneRecovery(scene);
}

enum class SeedScene { Manual, Auto, UnknownMode, Drift, Dark, Bright,
                       Expired, ControlWrite, PadFailure, IdentityReject, Cancel, Duplicate };

void TestManualWarmupSeed(SeedScene scene = SeedScene::Manual, int rotation = 0,
                          bool fastUnlock = true) {
    g_detectCalls = 0;
    g_padScore = scene == SeedScene::PadFailure ? 0.0f : 1.0f;
    unsigned long long sequence = 0;
    unsigned int bindings = 0;
    int firstMarker = 0;
    bool wrote = false, cancelled = false;
    bool duplicateReturned = false;
    std::vector<int> markers;
    std::vector<Clock::time_point> grabs;
    const auto result = Run(fastUnlock,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            if (bindings++ == 0) firstMarker = g_lastEmbeddingMarker;
            markers.push_back(g_lastEmbeddingMarker);
            if (scene == SeedScene::IdentityReject)
                return BindingDecision{BindingDecisionKind::Reject, L"test identity rejection"};
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }, [&] { return cancelled; }, [] { return false; }, ipc::AUTH_TIMEOUT_SECONDS,
        [] { return false; }, [&](AuthPipelineCallbacks& callbacks, AuthPipelineConfig& config) {
            config.cameraRotation = rotation;
            callbacks.sensorKnobs.exposureIsManual = [&](bool& manual) {
                manual = scene != SeedScene::Auto && !wrote;
                return scene != SeedScene::UnknownMode;
            };
            if (scene == SeedScene::Auto || scene == SeedScene::ControlWrite) {
                callbacks.sensorKnobs.exposureGet = [](long& value) { value = -8; return true; };
                callbacks.sensorKnobs.exposureSetAuto = [&](long) { wrote = true; return true; };
            }
            callbacks.grabFrame = [&](FrameImage& frame, unsigned long long& seq) {
                if (sequence == 1) {
                    const auto deadline = Clock::now() + std::chrono::seconds(1);
                    while (g_detectCalls == 0 && Clock::now() < deadline)
                        std::this_thread::yield();
                    // Simulate a slower camera cadence, letting the first
                    // asynchronous detection finish before sample two.
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        scene == SeedScene::Expired ? 450 : 40));
                    if (scene == SeedScene::Cancel) cancelled = true;
                    if (scene == SeedScene::Duplicate && !duplicateReturned) {
                        duplicateReturned = true;
                        frame = FrameImage(160, 160);
                        for (size_t i = 0; i < frame.size(); ++i) frame[i] = RgbPixel(80, 80, 80);
                        seq = sequence;
                        return true;
                    }
                }
                frame = FrameImage(160, 160);
                for (size_t i = 0; i < frame.size(); ++i) frame[i] = RgbPixel(80, 80, 80);
                seq = ++sequence;
                if (sequence == 2) {
                    const unsigned char luma = scene == SeedScene::Drift ? 95
                        : scene == SeedScene::Dark ? 45 : scene == SeedScene::Bright ? 125 : 80;
                    // Change only the face ROI; the whole-frame warmup still
                    // opens, so this exercises the separate face drift guard.
                    for (long y = 20; y < 120; ++y)
                        for (long x = 20; x < 120; ++x) frame(y, x) = RgbPixel(luma, luma, luma);
                }
                const RgbPixel marker(static_cast<unsigned char>(seq), 80, 80);
                frame(0, 0) = frame(0, 159) = frame(159, 0) = frame(159, 159) = marker;
                grabs.push_back(Clock::now());
                return true;
            };
        });
    if (scene == SeedScene::Cancel) {
        Check(result.cancelled && !result.succeeded && bindings == 0,
              "cancellation at the stable gate releases async detection without binding");
    } else {
        const bool mayReuse = scene == SeedScene::Manual || scene == SeedScene::Auto ||
            scene == SeedScene::PadFailure || scene == SeedScene::IdentityReject ||
            scene == SeedScene::Duplicate;
        Check(mayReuse ? firstMarker == 1 : firstMarker > 1,
              "only stable known-mode, fresh, unchanged warmup evidence is reused");
        if (scene == SeedScene::IdentityReject) {
            Check(!result.succeeded && bindings == 1 && result.errorMessage == L"test identity rejection",
                  "reused seed cannot bypass identity rejection");
        } else if (scene == SeedScene::PadFailure) {
            Check(!result.succeeded && bindings == 3, "reused seed cannot bypass failed PAD");
        } else {
            Check(result.succeeded && bindings == 3, "warmup seed preserves all three identity bindings");
        }
        for (size_t i = 1; i < markers.size(); ++i) {
            Check(markers[i] > markers[i - 1], "reused seed bindings use increasing distinct frames");
            Check(grabs[markers[i] - 1] - grabs[markers[i - 1] - 1] >= std::chrono::milliseconds(60),
                  "reused seed retains at least 60ms between bound-frame captures");
        }
    }
    g_padScore = 1.0f;
    std::printf("Warmup seed scene=%d rotation=%d fast=%d: first=%d, bindings=%u, success=%d\n",
                static_cast<int>(scene), rotation, fastUnlock, firstMarker, bindings, result.succeeded);
}

// Exit while SCRFD still owns its input. Preparation must join before Run
// returns, and neither cancellation nor expiry may produce identity evidence.
void TestPendingWarmupExit(bool expireDeadline) {
    g_detectCalls = 0;
    g_detectionDelayMs = 100;
    unsigned int bindings = 0;
    const auto result = Run(true,
        [&](const std::vector<float>&, unsigned int, float, float, float) {
            ++bindings;
            return BindingDecision{BindingDecisionKind::Accept, {}};
        }, [&] { return !expireDeadline && g_detectCalls.load() > 0; },
        [] { return false; }, ipc::AUTH_TIMEOUT_SECONDS,
        [] { return false; }, [&](AuthPipelineCallbacks&, AuthPipelineConfig& config) {
            if (expireDeadline) config.authDeadline = ipc::MakeAuthDeadline(30);
        });
    Check(g_detectCalls > 0 && g_activeDetections == 0,
          "pending warmup detection is joined before the pipeline returns");
    Check(!result.succeeded && bindings == 0 &&
          (expireDeadline ? result.timedOut : result.cancelled),
          "pending warmup exit preserves the shared deadline/cancel terminal state");
    g_detectionDelayMs = 0;
}

void TestWarmupSeeds() {
    TestPendingWarmupExit(false);
    TestPendingWarmupExit(true);
    for (auto scene : {SeedScene::Manual, SeedScene::Auto, SeedScene::UnknownMode,
                       SeedScene::Drift, SeedScene::Dark, SeedScene::Bright,
                       SeedScene::Expired, SeedScene::ControlWrite, SeedScene::PadFailure,
                       SeedScene::IdentityReject, SeedScene::Cancel, SeedScene::Duplicate})
        TestManualWarmupSeed(scene);
    TestManualWarmupSeed(SeedScene::Manual, 90);
    TestManualWarmupSeed(SeedScene::Drift, 90);
    TestManualWarmupSeed(SeedScene::Manual, 0, false);
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--tune-recovery") == 0) {
        TestTuneRecoveries();
        return g_failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--warmup-seed") == 0) {
        TestWarmupSeeds();
        return g_failures == 0 ? 0 : 1;
    }
    TestTuneRecoveries();
    TestWarmupSeeds();
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
    TestPacingSkipsStaleFramesAndRetainsRetryAnchor();
    TestFrozenCameraHonorsTimeout();
    TestFrozenCameraHonorsDisconnect();
    TestPreparationConsumesAuthBudget();
    TestExpiredHostDeadlineNeverBinds();
    if (g_failures == 0) std::puts("AuthPipelineTest: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
