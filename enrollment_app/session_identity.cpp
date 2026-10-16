#include "session_identity.h"
#include "../common/logger.h"

#include <windows.h>
#include <sddl.h>
#include <vector>

namespace facelogin::session_identity {

// ---------------------------------------------------------------------------
// Session identity helpers
//
// The authoritative UPN source is GetUserNameExW(NameUserPrincipal): it
// returns the email for Microsoft accounts and fails with ERROR_NONE_MAPPED
// (1332) for local accounts. On a subset of machines, however, the whole UPN
// family of name formats fails with 1332 even for a live MSA session
// (observed on Windows 11 with a local SAM account linked to a Microsoft
// account: only NameFullyQualifiedDN / NameSamCompatible succeed;
// UserPrincipal/Display/Canonical/CanonicalEx/SPN/DnsDomain all return 1332).
// In that case the UPN is empty and every MSA/local decision collapses to
// "local" — the UI then says "Windows 密码" while the password that actually
// authenticates is the Microsoft account password (the user-visible symptom
// we are fixing).
//
// Fallback: scan the process token's ENABLED groups for the MSA identity.
// LSA injects a group SID under the MicrosoftAccount\S-1-11-96-... authority
// into the token of any session that logged on via a linked Microsoft
// account, and LookupAccountSidW resolves its name to the email (e.g.
// "MicrosoftAccount\shi2jun2cheng2@outlook.com"). This is bound to the
// CURRENT session token, so unlike the machine-wide IdentityStore\LogonCache
// registry caches it can never carry another user's leftover MSA email — the
// exact misattribution that produced bug1's perpetual false "账号身份已变更"
// prompt (docs/todo.md bug1). IdentityStore\LogonCache\Name2Sid and
// IdentityCRL\StoredIdentities must NOT be re-added as fallbacks.
// ---------------------------------------------------------------------------

// Primary UPN query. Returns empty on failure (1332 or no secur32).
static std::wstring GetSessionUpn() {
    std::wstring upn;
    HMODULE hSecur32 = LoadLibraryW(L"secur32.dll");
    if (!hSecur32) return upn;
    typedef BOOLEAN (WINAPI *PFN_GetUserNameExW)(int, LPWSTR, PULONG);
    auto pfn = reinterpret_cast<PFN_GetUserNameExW>(
        GetProcAddress(hSecur32, "GetUserNameExW"));
    if (pfn) {
        ULONG upnSize = 256;
        std::vector<wchar_t> upnBuf(upnSize);
        if (pfn(8 /* NameUserPrincipal */, upnBuf.data(), &upnSize)) {
            upn = upnBuf.data();
        }
    }
    FreeLibrary(hSecur32);
    return upn;
}

// Fallback used only when GetSessionUpn() returned empty: scans the current
// process token's ENABLED groups for the MSA identity LSA injects and returns
// the email portion. Empty when the session is genuinely local (no MSA group)
// or the SID cannot be resolved.
//
// The S-1-11-96-... prefix (identifier authority 11, first sub-authority 96)
// is the MicrosoftAccount namespace — used as a cheap filter before the
// authoritative LookupAccountSidW call. We only consider enabled groups
// (SE_GROUP_ENABLED) and skip deny-only / disabled groups, since a disabled
// MSA group does not represent the active logon identity.
static std::wstring ResolveSessionMsaUpnFromToken() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    std::wstring result;
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenGroups, nullptr, 0, &sz);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetTokenInformation(hToken, TokenGroups, buf.data(), sz, &sz)) {
            auto* tg = reinterpret_cast<TOKEN_GROUPS*>(buf.data());
            for (DWORD i = 0; i < tg->GroupCount; ++i) {
                // Only enabled groups represent the active identity; skip
                // SE_GROUP_USE_FOR_DENY_ONLY and disabled groups.
                if (!(tg->Groups[i].Attributes & SE_GROUP_ENABLED)) {
                    continue;
                }
                PSID sid = tg->Groups[i].Sid;
                if (!sid) continue;

                // Cheap filter on the string form: S-1-11-96-... is the
                // MicrosoftAccount namespace.
                LPWSTR sidStr = nullptr;
                if (!ConvertSidToStringSidW(sid, &sidStr)) continue;
                std::wstring sidStrW = sidStr;
                LocalFree(sidStr);
                if (sidStrW.rfind(L"S-1-11-96-", 0) != 0) continue;

                // Authoritative check: domain must be "MicrosoftAccount" and
                // the name must contain '@' (the email form).
                wchar_t name[256] = {};
                wchar_t domain[256] = {};
                DWORD nameLen = ARRAYSIZE(name);
                DWORD domainLen = ARRAYSIZE(domain);
                SID_NAME_USE sidType;
                if (LookupAccountSidW(nullptr, sid, name, &nameLen,
                                      domain, &domainLen, &sidType)) {
                    std::wstring nameW = name;
                    std::wstring domainW = domain;
                    if (domainW == L"MicrosoftAccount" &&
                        nameW.find(L'@') != std::wstring::npos) {
                        result = nameW;
                        break;
                    }
                }
            }
        }
    }
    CloseHandle(hToken);
    return result;
}

// Unified entry point for ALL MSA/local decisions. Try the authoritative
// GetUserNameExW query first; on 1332/empty fall back to the token MSA group.
std::wstring ResolveSessionUpn() {
    std::wstring upn = GetSessionUpn();
    if (!upn.empty() && upn.find(L'@') != std::wstring::npos) {
        return upn;
    }
    return ResolveSessionMsaUpnFromToken();
}

// MSA test used everywhere: UPN non-empty and contains '@'.
bool IsMsaUpn(const std::wstring& upn) {
    return !upn.empty() && upn.find(L'@') != std::wstring::npos;
}

SessionIdentity ReadSessionIdentity() {
    SessionIdentity identity;
    // --- Gather user identity ---
    {
        wchar_t username[256] = {};
        DWORD size = ARRAYSIZE(username);
        if (!GetUserNameW(username, &size)) {
            DWORD err = GetLastError();
            FACELOGIN_ERROR(L"GetUserNameW FAILED: err=%lu", err);
            username[0] = L'\0';
        }
        identity.username = username;
    }

    // Get UPN (UserPrincipalName) — the ONLY trusted source of the current
    // session's identity. GetUserNameExW(NameUserPrincipal) returns the email
    // UPN for Microsoft accounts and fails with ERROR_NONE_MAPPED for local
    // accounts, so identity.upn stays empty for local users.
    //
    // NOTE: machine-wide registry fallbacks (IdentityCRL\StoredIdentities,
    // IdentityStore\LogonCache\Name2Sid) used to live here and adopted ANY
    // cached MSA email without verifying it belongs to the current user. On
    // machines with a leftover MSA identity (a previous user profile, or an
    // MSA later converted to local) that mislabeled local accounts as MSA and
    // poisoned the stored record — producing a perpetual false
    // "账号身份已变更" prompt (docs/todo.md bug1). Never re-add an
    // unattributed cache lookup.
    //
    // ResolveSessionUpn() additionally falls back to the process-token MSA
    // group when GetUserNameExW fails (see comment at its definition) — this
    // machine exhibits exactly that 1332 failure for the whole UPN family.
    identity.upn = ResolveSessionUpn();

    // Determine account type: MSA accounts have a UPN containing '@'
    identity.accountType = "local";
    if (IsMsaUpn(identity.upn)) {
        identity.accountType = "msa";
    }

    // Get SID via LookupAccountNameW
    {
        DWORD sidSize = 0, domainSize = 0;
        SID_NAME_USE sidType;
        std::wstring lookupName = identity.upn.empty() ? identity.username : identity.upn;

        LookupAccountNameW(nullptr, lookupName.c_str(),
                           nullptr, &sidSize, nullptr, &domainSize, &sidType);
        if (sidSize > 0) {
            std::vector<BYTE> sidBuf(sidSize);
            std::vector<wchar_t> domainBuf(domainSize > 0 ? domainSize : 1);
            if (LookupAccountNameW(nullptr, lookupName.c_str(),
                                   sidBuf.data(), &sidSize,
                                   domainBuf.data(), &domainSize, &sidType)) {
                LPWSTR sidStr = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf.data()), &sidStr)) {
                    identity.sid = sidStr;
                    LocalFree(sidStr);
                }
            } else {
                DWORD err = GetLastError();
                FACELOGIN_WARN(L"LookupAccountNameW(UPN) FAILED: err=%lu", err);
            }
        }

        if (identity.sid.empty()) {
            DWORD sidSize2 = 0, domainSize2 = 0;
            SID_NAME_USE sidType2;
            LookupAccountNameW(nullptr, identity.username.c_str(),
                               nullptr, &sidSize2, nullptr, &domainSize2, &sidType2);
            if (sidSize2 > 0) {
                std::vector<BYTE> sidBuf2(sidSize2);
                std::vector<wchar_t> domainBuf2(domainSize2 > 0 ? domainSize2 : 1);
                if (LookupAccountNameW(nullptr, identity.username.c_str(),
                                       sidBuf2.data(), &sidSize2,
                                       domainBuf2.data(), &domainSize2, &sidType2)) {
                    LPWSTR sidStr = nullptr;
                    if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf2.data()), &sidStr)) {
                        identity.sid = sidStr;
                        LocalFree(sidStr);
                    }
                } else {
                    DWORD err = GetLastError();
                    FACELOGIN_WARN(L"LookupAccountNameW(SAM) FAILED: err=%lu", err);
                }
            }
        }
    }

    // Identity summary — referenced by the MSA-detection verification steps.
    // SID logs as RID tail only, matching the service binding lines.
    std::wstring sidLog;
    if (identity.sid.empty()) {
        sidLog = L"<empty>";
    } else {
        const size_t lastDash = identity.sid.rfind(L'-');
        sidLog = L"…-" +
                 (lastDash == std::wstring::npos ? identity.sid : identity.sid.substr(lastDash + 1));
    }
    FACELOGIN_INFO(L"Session identity: username=%s upn=%s sid=%s accountType=%hs",
                   identity.username.c_str(),
                   identity.upn.empty() ? L"<empty>" : identity.upn.c_str(),
                   sidLog.c_str(),
                   identity.accountType.c_str());

    return identity;
}

// Returns the SID of the currently logged-on session identity (the process
// token's user), used to authorize face-management operations for the current
// logged-on account.
std::wstring GetCurrentProcessUserSid() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    std::wstring result;
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &sz);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetTokenInformation(hToken, TokenUser, buf.data(), sz, &sz)) {
            auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
            LPWSTR sidStr = nullptr;
            if (ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
                result = sidStr;
                LocalFree(sidStr);
            }
        }
    }
    CloseHandle(hToken);
    return result;
}

bool ValidatePassword(const SessionIdentity& identity,
                      const std::wstring& password) {
    // The MSA/local decision MUST come from the live session identity
    // (ResolveSessionUpn), never from the stored record: a registry fallback
    // used to pollute the stored UPN with another account's email
    // (docs/todo.md bug1), which routed local users into the MSA path below
    // and made the account refresh loop impossible to complete.
    std::wstring sessionUpn = ResolveSessionUpn();
    bool isMsa = IsMsaUpn(sessionUpn);

    if (isMsa) {
        // MSA: domain=NULL routes through CloudAP for an online validation of
        // the CURRENT password. (LogonUserW with domain="." would validate
        // against only the local account database — i.e. the STALE cached MSA
        // credential, not the user's current Microsoft password.) INTERACTIVE
        // also refreshes the cached credential when it succeeds (NETWORK
        // never caches).
        HANDLE hToken = nullptr;
        BOOL ok = LogonUserW(sessionUpn.c_str(), nullptr, password.c_str(),
                             LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken);
        if (ok && hToken) {
            CloseHandle(hToken);
            FACELOGIN_INFO(L"ValidatePassword: MSA online validation OK (%s)", sessionUpn.c_str());
            return true;
        }
        DWORD err = GetLastError();
        FACELOGIN_WARN(L"ValidatePassword: MSA interactive logon failed for %s (err=%lu)",
                       sessionUpn.c_str(), err);
        return false;
    }

    // Local account: validate against the local SAM database (reliable offline).
    HANDLE hToken = nullptr;
    BOOL ok = LogonUserW(identity.username.c_str(), L".", password.c_str(),
                         LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
    if (ok && hToken) {
        CloseHandle(hToken);
        return true;
    }
    FACELOGIN_WARN(L"ValidatePassword: local logon failed for %s (err=%lu)",
                   identity.username.c_str(), GetLastError());
    return false;
}

} // namespace facelogin::session_identity
