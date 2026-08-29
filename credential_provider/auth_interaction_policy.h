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

// Passive input polling runs on the initial Waiting round AND on the
// in-place failure tile, where a qualifying press (keyboard key or mouse
// button — the same wave gate as the first attempt, movement alone never
// qualifies) is routed to an explicit retry. Password-entry keystrokes stay
// excluded STRUCTURALLY, not by freezing detection: the watcher is stopped in
// SetDeselected before the user can type anywhere else, and this tile has no
// editable field, so a password can only be entered after the watcher is
// gone. Detection must still never be started from Advise alone — Advise
// fires for flows that never select this tile (PIN reset wizard) and the
// watcher polls GLOBAL input.
constexpr bool ShouldStartInputDetection(AuthState state) noexcept {
    return state == AuthState::Waiting || IsRetryableFailure(state);
}

// Only success needs provider re-enumeration so LogonUI asks for serialized
// credentials. Failures are rendered in-place and require an explicit retry.
constexpr bool ShouldReenumerateAfterTerminal(AuthState state) noexcept {
    return state == AuthState::Ready;
}

// Deselecting the tile must abort an in-flight recognition and return to the
// passive waiting state, so the camera is released as soon as the user moves
// to another sign-in option. Terminal failures keep their state — the
// explicit-retry rule survives a tile round-trip.
constexpr bool ShouldAbortAuthOnDeselect(AuthState state) noexcept {
    return state == AuthState::Authenticating;
}

// Pipe results that arrive after authentication was aborted (tile
// deselected, pipe torn down) are stale: they must not overwrite the reset
// state or push tile text. Only a live round may process responses.
constexpr bool ShouldProcessPipeResponse(AuthState state) noexcept {
    return state == AuthState::Authenticating;
}

} // namespace facelogin::credential_provider
