#pragma once

namespace facelogin::credential_provider {

// Keep the lock-screen interaction policy independent from COM plumbing so
// its security/usability invariants can be tested without loading LogonUI.
enum class AuthState {
    Waiting,
    Authenticating,
    Ready,
    Failed,
    Error
};

constexpr bool IsRetryableFailure(AuthState state) noexcept {
    return state == AuthState::Failed || state == AuthState::Error;
}

// Passive GetLastInputInfo polling is allowed only for the first attempt in a
// credential session. After a terminal failure, password-entry keystrokes must
// never be interpreted as another face-auth request.
constexpr bool ShouldStartInputDetection(AuthState state) noexcept {
    return state == AuthState::Waiting;
}

// Only success needs provider re-enumeration so LogonUI asks for serialized
// credentials. Failures are rendered in-place and require an explicit retry.
constexpr bool ShouldReenumerateAfterTerminal(AuthState state) noexcept {
    return state == AuthState::Ready;
}

} // namespace facelogin::credential_provider
