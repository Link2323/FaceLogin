#pragma once

namespace facelogin {

// Internal terminal outcome shared by inference and parent supervision.
// Default to failure so an early return can never imply authentication success.
// This enum is not serialized; the existing IPC terminal messages remain the
// wire contract.
enum class AuthOutcome {
    Failed,
    Succeeded,
    Cancelled,
    TimedOut,
};

} // namespace facelogin
