// Development-only liveness fast-fail timing state machine test.
// Covers the unlock-flow TODO 6 acceptance: the 2 s persistent-attack timer
// must time from the first detected face, never from the PAD window start.
#include "liveness_types.h"

#include <chrono>
#include <cstdio>

namespace {

using facelogin::LivenessTiming;
using TimePoint = std::chrono::steady_clock::time_point;

int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

TimePoint At(double seconds) {
    return TimePoint{} + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(seconds));
}

// Acceptance 1: a face first appearing at 0.0 / 1.9 / 2.1 / 2.4 s after the
// PAD window start must still reach PAD inference — no attack verdict before
// the first inference. 2.1 and 2.4 are the old-bug regression points.
void TestLateFaceStillReachesPadInference() {
    for (const double firstFace : {0.0, 1.9, 2.1, 2.4}) {
        LivenessTiming timing{At(0.0)};
        Check(!timing.ShouldFailEmptyScene(At(firstFace)),
              "empty-scene fast fail does not fire before 2.5s");
        timing.OnFaceDetected(At(firstFace));
        Check(!timing.ShouldFailPersistentAttack(At(firstFace)),
              "first detected face reaches PAD inference, not attack verdict");
        Check(!timing.ShouldFailEmptyScene(At(7.9)),
              "empty-scene fast fail never fires once a face has been seen");
    }
}

// Acceptance 2: the empty-scene fast fail still fires 2.4–2.7 s after the
// PAD window start when no face ever appears.
void TestEmptySceneFastFailUnchanged() {
    LivenessTiming timing{At(0.0)};
    Check(!timing.ShouldFailEmptyScene(At(2.4)),
          "empty scene still passing at 2.4s");
    Check(timing.ShouldFailEmptyScene(At(2.6)),
          "empty scene fails fast by 2.6s");
    Check(!timing.WindowExpired(At(7.9)), "8s PAD window open at 7.9s");
    Check(timing.WindowExpired(At(8.0)), "8s PAD window expires at 8.0s");
}

// Acceptance 3: continuously failing scores enter the attack verdict 1.9–2.2 s
// after the FIRST detected face, not after the window start.
void TestPersistentAttackTimesFromFirstFace() {
    LivenessTiming timing{At(0.0)};
    timing.OnFaceDetected(At(1.0));
    for (double t = 1.0; t < 2.9; t += 0.1) {
        timing.OnPadScore(false);
        Check(!timing.ShouldFailPersistentAttack(At(t)),
              "attack verdict does not fire before 2s of failing scores");
    }
    timing.OnPadScore(false);
    Check(timing.ShouldFailPersistentAttack(At(3.1)),
          "attack verdict fires ~2s after first face with all scores failing");
}

// Acceptance 4: one passing score permanently disables the attack fast fail;
// an all-fail sequence afterwards is bounded only by the 8 s window.
void TestPassDisablesAttackFastFail() {
    LivenessTiming timing{At(0.0)};
    timing.OnFaceDetected(At(0.5));
    timing.OnPadScore(true);
    for (double t = 0.75; t < 7.9; t += 0.25) {
        timing.OnPadScore(false);
        Check(!timing.ShouldFailPersistentAttack(At(t)),
              "attack fast fail stays off after the first PAD pass");
    }
    Check(timing.WindowExpired(At(8.0)),
          "all-fail after a pass is bounded by the 8s window instead");
    Check(timing.PassCount() == 1, "failed scores do not change the pass tally");
}

// The timer base is the FIRST detection: a face that vanishes and reappears
// still measures from its first appearance.
void TestTimerBaseIsFirstDetection() {
    LivenessTiming timing{At(0.0)};
    timing.OnFaceDetected(At(1.0));
    timing.OnFaceDetected(At(4.0));
    Check(timing.ShouldFailPersistentAttack(At(4.1)),
          "reappearing face still times from its first detection");
    Check(timing.AnyFaceSeen(), "any-face-seen latch survives reappearance");
}

} // namespace

int main() {
    TestLateFaceStillReachesPadInference();
    TestEmptySceneFastFailUnchanged();
    TestPersistentAttackTimesFromFirstFace();
    TestPassDisablesAttackFastFail();
    TestTimerBaseIsFirstDetection();
    if (g_failures == 0) {
        std::printf("LivenessTimingTest: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
