#pragma once

#include <string>

namespace facelogin {

// Resolves the runtime data directory with a security whitelist check.
//
// Trust model (defense against DataPath registry redirection — security #3):
//   The allowed data directory depends on where the running EXE lives.
//
//   Production mode (EXE under C:\Program Files\ or C:\Program Files (x86)\,
//   which are ACL-protected against non-admin writes):
//     Only the EXE's own directory is trusted. The registry DataPath value
//     MUST point there; any other value (including the %ProgramData%\FaceLogin
//     fallback) is rejected. This closes the redirection attack surface: an
//     attacker who can change DataPath gains nothing, because the only
//     permitted target is already the ACL-protected install directory.
//
//   Development mode (EXE elsewhere, e.g. build\):
//     The EXE directory, %ProgramData%\FaceLogin, and the caller-supplied
//     dataDirHint are all trusted. This keeps standalone testing ergonomic
//     on developer machines that are single-user and trusted.
//
// dataDirHint: caller-supplied override (non-empty in dev harnesses; empty
//              in production, where the registry is authoritative).
// outReason:   if the function returns empty, set to a short reason string
//              suitable for logging.
// Returns:     the trusted data directory, or empty on rejection (fail-closed).
std::wstring ResolveSecureDataDir(const std::wstring& dataDirHint,
                                  std::wstring* outReason = nullptr);

} // namespace facelogin
