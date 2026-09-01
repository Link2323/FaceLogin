#include "auth_interaction_policy.h"
#include "input_trigger_policy.h"

#include <iostream>

using facelogin::credential_provider::AuthState;
using facelogin::credential_provider::IsRetryableFailure;
using facelogin::credential_provider::ShouldAbortAuthOnDeselect;
using facelogin::credential_provider::ShouldProcessPipeResponse;
using facelogin::credential_provider::ShouldReenumerateAfterTerminal;
using facelogin::credential_provider::ShouldStartInputDetection;
using facelogin::credential_provider::InputDetectionRound;
using facelogin::credential_provider::InputTriggerKind;
using facelogin::credential_provider::InputTriggerPolicy;

static_assert(ShouldStartInputDetection(AuthState::Waiting));
static_assert(!ShouldStartInputDetection(AuthState::Authenticating));
// Failure tiles keep passive detection armed: a qualifying press routes to
// an explicit retry, and password keystrokes stay excluded structurally
// (watcher stopped on deselect; no editable field in this tile).
static_assert(ShouldStartInputDetection(AuthState::Failed));
static_assert(ShouldStartInputDetection(AuthState::Error));
static_assert(!ShouldStartInputDetection(AuthState::Ready));

static_assert(IsRetryableFailure(AuthState::Failed));
static_assert(IsRetryableFailure(AuthState::Error));
static_assert(!IsRetryableFailure(AuthState::Waiting));
static_assert(!IsRetryableFailure(AuthState::Ready));

static_assert(ShouldReenumerateAfterTerminal(AuthState::Ready));
static_assert(!ShouldReenumerateAfterTerminal(AuthState::Failed));
static_assert(!ShouldReenumerateAfterTerminal(AuthState::Error));

static_assert(ShouldAbortAuthOnDeselect(AuthState::Authenticating));
static_assert(!ShouldAbortAuthOnDeselect(AuthState::Waiting));
static_assert(!ShouldAbortAuthOnDeselect(AuthState::Ready));
static_assert(!ShouldAbortAuthOnDeselect(AuthState::Failed));
static_assert(!ShouldAbortAuthOnDeselect(AuthState::Error));

static_assert(ShouldProcessPipeResponse(AuthState::Authenticating));
static_assert(!ShouldProcessPipeResponse(AuthState::Waiting));
static_assert(!ShouldProcessPipeResponse(AuthState::Ready));
static_assert(!ShouldProcessPipeResponse(AuthState::Failed));
static_assert(!ShouldProcessPipeResponse(AuthState::Error));

namespace {

constexpr std::uint16_t kA = 0x41;
constexpr std::uint16_t kL = 0x4C;
constexpr std::uint16_t kLWin = 0x5B;
constexpr std::uint16_t kShift = 0x10;
constexpr std::uint16_t kLeftButton = 0x01;

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

bool TestInitialKeyboardEdges() {
    bool ok = true;

    InputTriggerPolicy wallpaperBreak(InputDetectionRound::InitialLock);
    ok &= Expect(wallpaperBreak.ObserveKey(kA, false),
                 "ordinary orphan BREAK must trigger the first wallpaper press");

    InputTriggerPolicy visibleMake(InputDetectionRound::InitialLock);
    ok &= Expect(visibleMake.ObserveKey(kA, true),
                 "a visible credential-view MAKE must trigger immediately");
    ok &= Expect(!visibleMake.ObserveKey(kA, false),
                 "one press must trigger at most once");

    InputTriggerPolicy modifierBreak(InputDetectionRound::InitialLock);
    ok &= Expect(modifierBreak.ObserveKey(kShift, false),
                 "a non-Windows modifier wallpaper press must still trigger once");

    return ok;
}

bool TestMousePolicy() {
    bool ok = true;

    InputTriggerPolicy movementOnly(InputDetectionRound::InitialLock);
    // Movement deliberately has no policy API: doing nothing must not trigger.
    ok &= Expect(!movementOnly.triggered(), "mouse movement must never trigger");

    InputTriggerPolicy click(InputDetectionRound::InitialLock);
    ok &= Expect(!click.ObserveMouseButton(kLeftButton, true),
                 "mouse DOWN alone must not trigger before LogonUI handles the click");
    ok &= Expect(!click.triggered(), "mouse DOWN must leave the policy untriggered");
    ok &= Expect(click.ObserveMouseButton(kLeftButton, false),
                 "a complete visible mouse click must trigger on button UP");
    ok &= Expect(click.triggerKind() == InputTriggerKind::MouseButton,
                 "a complete click must retain mouse trigger provenance");

    InputTriggerPolicy inheritedClick(InputDetectionRound::FailureRetry);
    inheritedClick.SeedMouseButtonDown(kLeftButton);
    ok &= Expect(!inheritedClick.ObserveMouseButton(kLeftButton, true),
                 "a button held across arm must not trigger");
    ok &= Expect(!inheritedClick.ObserveMouseButton(kLeftButton, false),
                 "the inherited button release must not trigger");
    ok &= Expect(!inheritedClick.ObserveMouseButton(kLeftButton, true),
                 "the next new click DOWN must wait for completion");
    ok &= Expect(inheritedClick.ObserveMouseButton(kLeftButton, false),
                 "the next complete click must trigger");

    return ok;
}

bool TestWinLReleaseOrders() {
    bool ok = true;

    // Hold Win+L, then release both (L first in this concrete ordering).
    InputTriggerPolicy together(InputDetectionRound::InitialLock);
    together.ObserveKey(kL, true);
    together.ObserveKey(kL, true);  // auto-repeat
    ok &= Expect(!together.ObserveKey(kL, false),
                 "Win+L L release must be drained");
    ok &= Expect(!together.ObserveKey(kLWin, false),
                 "orphan Win release must be drained");
    ok &= Expect(!together.triggered(), "Win+L release must not trigger auth");
    ok &= Expect(together.ObserveKey(kA, true),
                 "the first new key after Win+L must trigger");

    // Release Win, continue holding/repeating L, then release L.
    InputTriggerPolicy winFirst(InputDetectionRound::InitialLock);
    winFirst.ObserveKey(kL, true);
    ok &= Expect(!winFirst.ObserveKey(kLWin, false),
                 "Win-first release must not trigger");
    winFirst.ObserveKey(kL, true);
    winFirst.ObserveKey(kL, true);
    ok &= Expect(!winFirst.ObserveKey(kL, false),
                 "the later L release must not trigger");
    ok &= Expect(!winFirst.triggered(),
                 "Win-first long-hold sequence must not trigger auth");

    // Release L first, then release the non-repeating Win key later.
    InputTriggerPolicy lFirst(InputDetectionRound::InitialLock);
    lFirst.ObserveKey(kL, true);
    lFirst.ObserveKey(kL, true);
    ok &= Expect(!lFirst.ObserveKey(kL, false),
                 "L-first release must not trigger");
    ok &= Expect(!lFirst.ObserveKey(kLWin, false),
                 "the delayed Win release must not trigger");
    ok &= Expect(!lFirst.triggered(),
                 "L-first long-hold sequence must not trigger auth");

    return ok;
}

bool TestConservativeInitialLAndRetry() {
    bool ok = true;

    InputTriggerPolicy initialL(InputDetectionRound::InitialLock);
    ok &= Expect(!initialL.ObserveKey(kL, true),
                 "the ambiguous first L MAKE must be drained");
    ok &= Expect(!initialL.ObserveKey(kL, false),
                 "the ambiguous first L BREAK must be drained");
    ok &= Expect(initialL.ObserveKey(kL, true),
                 "a second, demonstrably new L press must trigger");

    InputTriggerPolicy retry(InputDetectionRound::FailureRetry);
    retry.SeedKeyDown(kA);
    ok &= Expect(!retry.ObserveKey(kA, true),
                 "retry must ignore inherited-key auto-repeat");
    ok &= Expect(!retry.ObserveKey(kA, false),
                 "retry must ignore inherited-key release");
    ok &= Expect(retry.ObserveKey(kA, true),
                 "retry must accept the next fresh MAKE");

    InputTriggerPolicy orphanRetryBreak(InputDetectionRound::FailureRetry);
    ok &= Expect(!orphanRetryBreak.ObserveKey(kA, false),
                 "failure tile must reject an orphan BREAK from the old round");

    InputTriggerPolicy retryL(InputDetectionRound::FailureRetry);
    ok &= Expect(retryL.ObserveKey(kL, true),
                 "failure tile must not apply the initial-lock L exception");

    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= TestInitialKeyboardEdges();
    ok &= TestMousePolicy();
    ok &= TestWinLReleaseOrders();
    ok &= TestConservativeInitialLAndRetry();
    if (!ok) return 1;

    std::cout << "Credential Provider interaction policy tests passed\n";
    return 0;
}
