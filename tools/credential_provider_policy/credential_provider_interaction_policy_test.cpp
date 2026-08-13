#include "auth_interaction_policy.h"

#include <iostream>

using facelogin::credential_provider::AuthState;
using facelogin::credential_provider::IsRetryableFailure;
using facelogin::credential_provider::ShouldReenumerateAfterTerminal;
using facelogin::credential_provider::ShouldStartInputDetection;

static_assert(ShouldStartInputDetection(AuthState::Waiting));
static_assert(!ShouldStartInputDetection(AuthState::Authenticating));
static_assert(!ShouldStartInputDetection(AuthState::Failed));
static_assert(!ShouldStartInputDetection(AuthState::Error));
static_assert(!ShouldStartInputDetection(AuthState::Blocked));

static_assert(IsRetryableFailure(AuthState::Failed));
static_assert(IsRetryableFailure(AuthState::Error));
static_assert(!IsRetryableFailure(AuthState::Waiting));
static_assert(!IsRetryableFailure(AuthState::Ready));
static_assert(!IsRetryableFailure(AuthState::Blocked));

static_assert(ShouldReenumerateAfterTerminal(AuthState::Ready));
static_assert(!ShouldReenumerateAfterTerminal(AuthState::Failed));
static_assert(!ShouldReenumerateAfterTerminal(AuthState::Error));
static_assert(!ShouldReenumerateAfterTerminal(AuthState::Blocked));

int main() {
    std::cout << "Credential Provider interaction policy tests passed\n";
    return 0;
}
