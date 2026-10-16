#pragma once

#include <string>

namespace facelogin {

enum class EnrollmentLogSource { Console, Service, AuthWorker, CredentialProvider };

// Reads the latest 256 KiB as a JSON line array, including legacy UTF-16 logs.
std::string ReadEnrollmentLog(const std::wstring& dataDir, EnrollmentLogSource source);
// Clears only the active file; rotated logs remain. Missing files are success.
// Console also clears the process logger's in-memory ring.
bool ClearEnrollmentLog(const std::wstring& dataDir, EnrollmentLogSource source);

} // namespace facelogin
