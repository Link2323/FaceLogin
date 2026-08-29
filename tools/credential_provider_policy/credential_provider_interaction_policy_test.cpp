#include "auth_interaction_policy.h"

#include <iostream>

using facelogin::credential_provider::AuthState;
using facelogin::credential_provider::IsRetryableFailure;
using facelogin::credential_provider::ShouldAbortAuthOnDeselect;
using facelogin::credential_provider::ShouldProcessPipeResponse;
using facelogin::credential_provider::ShouldReenumerateAfterTerminal;
using facelogin::credential_provider::ShouldStartInputDetection;

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

int main() {
    std::cout << "Credential Provider interaction policy tests passed\n";
    return 0;
}
