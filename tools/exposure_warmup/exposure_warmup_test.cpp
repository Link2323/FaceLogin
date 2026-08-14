// Development-only exposure warmup state machine test. The gate must:
//   - open after 2 distinct stable samples in a settled scene (stricter than
//     the legacy 3-iteration discard, which usually saw ~1 distinct frame);
//   - extend while AGC ramps and open on the first stable window;
//   - cap at 10 samples and proceed anyway when it never settles;
//   - not be locked by an early outlier — the stream recovers once the
//     outlier leaves the rolling window;
//   - sample only what MeanLuma reports (Rec.601, subsampled).
//
// `--camera` runs the same warmup loop as AuthPipeline::Run against the real
// DirectShow camera (no service / admin rights needed): verifies the frame
// sequence is monotonic across sampled frames, that duplicate buffered
// frames are skipped, and that the gate opens within the sample bounds.
#include "exposure_warmup.h"

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <thread>

#ifndef EXPOSURE_WARMUP_TEST_NO_CAMERA
#include "webcam_capture_dshow.h"
#endif

namespace {

using facelogin::ExposureWarmup;
using facelogin::ExposureWarmupConfig;
using facelogin::FrameImage;
using facelogin::MeanLuma;
using facelogin::RgbPixel;

int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

void CheckClose(float actual, float expected, const char* description) {
    const float delta = actual - expected > 0 ? actual - expected : expected - actual;
    if (delta > 0.1f) {
        std::fprintf(stderr, "FAIL: %s (got %.2f, want %.2f)\n", description, actual, expected);
        ++g_failures;
    }
}

// Acceptance 1: a settled scene exits at the 2-sample floor.
void TestSettledSceneExitsAtTwoSampleFloor() {
    ExposureWarmup warmup;
    Check(!warmup.Feed(100.0f), "settled scene: sample 1 does not open the gate");
    Check(warmup.Feed(101.0f), "settled scene: sample 2 opens the gate");
    Check(warmup.Samples() == 2, "settled scene consumed exactly 2 samples");
    Check(warmup.Settled(), "settled scene exits via stability, not cap");
}

// Acceptance 2: an AGC ramp (dark → lit) keeps the gate closed until the
// first stable window completes. Expected trace with window=2, tol=20:
// (40,70) unstable, (70,100) unstable, (100,105) stable → 4.
void TestRampThenStable() {
    ExposureWarmup warmup;
    Check(!warmup.Feed(40.0f), "ramp: sample 1 closed");
    Check(!warmup.Feed(70.0f), "ramp: sample 2 closed (window still spread)");
    Check(!warmup.Feed(100.0f), "ramp: sample 3 closed");
    Check(warmup.Feed(105.0f), "ramp: sample 4 opens the gate");
    Check(warmup.Samples() == 4, "ramp consumed 4 samples");
    Check(warmup.Settled(), "ramp exits via stability");
}

// Acceptance 3: a scene that never settles (hard oscillation) hits the
// 10-sample cap and reports not-settled — the auth loop proceeds anyway.
void TestNeverStableHitsCap() {
    ExposureWarmup warmup;
    for (int i = 0; i < 9; ++i) {
        Check(!warmup.Feed(i % 2 == 0 ? 20.0f : 120.0f), "oscillation stays closed below the cap");
    }
    Check(warmup.Feed(120.0f), "oscillation opens at the 10-sample cap");
    Check(warmup.Samples() == 10, "cap consumed exactly 10 samples");
    Check(!warmup.Settled(), "cap exit is not a stability exit");
}

// Acceptance 4: an outlier inside the current window (hand over the lens,
// light flicker) delays the exit; once it rolls out of the window, a stable
// pair opens the gate.
void TestOutlierInWindowDelaysExit() {
    ExposureWarmup warmup;
    Check(!warmup.Feed(100.0f), "outlier stream: sample 1 closed");
    Check(!warmup.Feed(250.0f), "outlier stream: outlier sample closed");
    Check(!warmup.Feed(100.0f), "outlier stream: sample 3 still pairs with outlier");
    Check(warmup.Feed(101.0f), "outlier stream: sample 4 opens once outlier left window");
    Check(warmup.Samples() == 4, "outlier stream consumed 4 samples");
    Check(warmup.Settled(), "outlier stream exits via stability");
}

// Acceptance 5: minSamples gates the exit independently of the window. With
// window=2, stability is reachable at sample 2, but the gate must stay
// closed until minSamples=5.
void TestMinSamplesFloor() {
    ExposureWarmupConfig config;
    config.minSamples = 5;
    config.window = 2;
    ExposureWarmup warmup(config);
    Check(!warmup.Feed(100.0f), "floor: sample 1 closed");
    Check(!warmup.Feed(100.0f), "floor: window full but below minSamples");
    Check(!warmup.Feed(100.0f), "floor: sample 3 closed");
    Check(!warmup.Feed(100.0f), "floor: sample 4 closed");
    Check(warmup.Feed(100.0f), "floor: sample 5 opens (minSamples reached, window stable)");
    Check(warmup.Samples() == 5, "floor consumed exactly 5 samples");
}

// Acceptance 6: MeanLuma reports subsampled Rec.601 luma. Sizes deliberately
// not multiples of 4 to exercise the subsampling loop bounds.
void TestMeanLuma() {
    FrameImage white(9, 13);
    for (long i = 0; i < static_cast<long>(white.size()); ++i) white[i] = RgbPixel(255, 255, 255);
    CheckClose(MeanLuma(white), 255.0f, "MeanLuma white");

    FrameImage black(9, 13);
    CheckClose(MeanLuma(black), 0.0f, "MeanLuma black (default-initialized)");

    FrameImage red(9, 13);
    for (long i = 0; i < static_cast<long>(red.size()); ++i) red[i] = RgbPixel(255, 0, 0);
    CheckClose(MeanLuma(red), 76.2f, "MeanLuma pure red (0.299*255)");

    FrameImage gray(9, 13);
    for (long i = 0; i < static_cast<long>(gray.size()); ++i) gray[i] = RgbPixel(60, 60, 60);
    CheckClose(MeanLuma(gray), 60.0f, "MeanLuma mid gray");

    CheckClose(MeanLuma(FrameImage()), 0.0f, "MeanLuma empty frame is 0");
}

} // namespace

#ifndef EXPOSURE_WARMUP_TEST_NO_CAMERA
// Live DirectShow probe mirroring the warmup loop in AuthPipeline::Run
// verbatim (same constants, same duplicate-frame skip, same pacing).
int RunLiveCameraWarmup() {
    using facelogin::WebcamCaptureDS;
    if (!WebcamCaptureDS::InitializeCOM()) {
        std::printf("camera mode: COM init failed\n");
        return 1;
    }
    WebcamCaptureDS camera;
    if (!camera.Initialize(640, 480, L"")) {
        std::printf("camera mode: no camera available\n");
        WebcamCaptureDS::ShutdownCOM();
        return 1;
    }

    const facelogin::ExposureWarmupConfig warmupConfig;
    facelogin::ExposureWarmup warmup(warmupConfig);
    const auto start = std::chrono::steady_clock::now();
    unsigned long long lastSeq = 0;
    bool haveLastSeq = false;
    int duplicateGrabs = 0;
    int emptyGrabs = 0;
    bool gateOpen = false;
    for (int attempts = 0; attempts < warmupConfig.maxAttempts && !gateOpen; ++attempts) {
        facelogin::FrameImage frame;
        unsigned long long seq = 0;
        if (!camera.GrabFrame(frame, &seq)) {
            ++emptyGrabs;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (haveLastSeq && seq == lastSeq) {
            ++duplicateGrabs;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Sampled frames must carry a strictly increasing sequence — the
        // monotonicity the exposure warmup's dedupe depends on.
        if (haveLastSeq && seq < lastSeq) {
            std::printf("FAIL: frame sequence went backwards (%llu -> %llu)\n",
                        lastSeq, seq);
            camera.Shutdown();
            WebcamCaptureDS::ShutdownCOM();
            return 1;
        }
        haveLastSeq = true;
        lastSeq = seq;
        const float luma = facelogin::MeanLuma(frame);
        std::printf("  sample %d: seq=%llu luma=%.1f\n", warmup.Samples() + 1, seq, luma);
        gateOpen = warmup.Feed(luma);
        if (!gateOpen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    const auto elapsedMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start).count();
    camera.Shutdown();
    WebcamCaptureDS::ShutdownCOM();

    std::printf("camera mode: gate %s after %d samples in %.0f ms (%d duplicate, %d empty grabs)\n",
                warmup.Settled() ? "STABLE" : "CAP", warmup.Samples(), elapsedMs,
                duplicateGrabs, emptyGrabs);
    const bool pass = gateOpen && warmup.Samples() >= warmupConfig.minSamples &&
                      warmup.Samples() <= warmupConfig.maxSamples;
    if (!pass) {
        std::printf("FAIL: live warmup did not open within bounds\n");
        return 1;
    }
    std::printf("camera mode: all invariants passed\n");
    return 0;
}
#endif

int wmain(int argc, wchar_t** argv) {
#ifndef EXPOSURE_WARMUP_TEST_NO_CAMERA
    if (argc == 2 && std::wcscmp(argv[1], L"--camera") == 0) {
        return RunLiveCameraWarmup();
    }
#endif

    TestSettledSceneExitsAtTwoSampleFloor();
    TestRampThenStable();
    TestNeverStableHitsCap();
    TestOutlierInWindowDelaysExit();
    TestMinSamplesFloor();
    TestMeanLuma();

    if (g_failures == 0) {
        std::printf("ExposureWarmupTest: all checks passed\n");
        return 0;
    }
    std::printf("ExposureWarmupTest: %d check(s) failed\n", g_failures);
    return 1;
}
