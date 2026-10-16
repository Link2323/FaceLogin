#pragma once

#include <string>

namespace facelogin::session_identity {

// Snapshot used for display and locating enrollment records. Live account
// changes and password checks re-query the process session, not users.dat.
struct SessionIdentity {
    std::wstring username;
    std::wstring upn;
    std::wstring sid;
    std::string accountType;
};

SessionIdentity ReadSessionIdentity();
std::wstring ResolveSessionUpn();
std::wstring GetCurrentProcessUserSid();
bool IsMsaUpn(const std::wstring& upn);
// Borrows the password for this call; never stores or logs it.
bool ValidatePassword(const SessionIdentity& identity, const std::wstring& password);

} // namespace facelogin::session_identity
