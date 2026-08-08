#pragma once

// Secure clearing of sensitive std::wstring buffers (passwords).
//
// SecureZeroMemory overwrites the buffer with zeros (resisting optimizer
// elision that a plain memset would suffer) and .clear() resets the length
// so nothing about the old contents remains reachable. Use SecureWStringGuard
// at function entry to guarantee every return path clears the password —
// manual clearing on the success path alone leaves plaintext in memory when
// an early return fires (security #9).

#include <windows.h>
#include <string>

namespace facelogin {

// Zeroes and clears a wstring in place.
inline void SecureClearWString(std::wstring& s) {
    if (!s.empty()) {
        SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
        s.clear();
    }
}

// RAII guard: clears the bound wstring on destruction, covering every exit
// path from the enclosing scope. Call release() to hand off ownership (the
// string is then left untouched on scope exit).
class SecureWStringGuard {
public:
    explicit SecureWStringGuard(std::wstring& s) : m_s(s) {}
    ~SecureWStringGuard() {
        if (m_active) SecureClearWString(m_s);
    }
    SecureWStringGuard(const SecureWStringGuard&) = delete;
    SecureWStringGuard& operator=(const SecureWStringGuard&) = delete;
    void release() { m_active = false; }
private:
    std::wstring& m_s;
    bool m_active = true;
};

} // namespace facelogin
