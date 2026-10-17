#pragma once

// Single implementation of JSON string-literal escaping for the JSON
// producers: the service's LEARNING_STATUS body (wide), and the enrollment
// face list / account-change / camera-list responses (narrow UTF-8).
// config_util.cpp's JSON *parser* performs the reverse direction and is
// intentionally separate.

#include <cstdio>
#include <string>

#include "text_util.h"

namespace facelogin {

// Escapes a wide string for embedding inside a JSON string literal per
// RFC 8259: quote, backslash and the mandatory control characters
// (U+0000-U+001F, using the \b\f\n\r\t short forms). Forward slash is
// intentionally NOT escaped — legal unescaped and never ambiguous for
// these fields.
inline std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 2);
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') {
            out.push_back(L'\\');
            out.push_back(c);
        } else if (c == L'\b') {
            out += L"\\b";
        } else if (c == L'\f') {
            out += L"\\f";
        } else if (c == L'\n') {
            out += L"\\n";
        } else if (c == L'\r') {
            out += L"\\r";
        } else if (c == L'\t') {
            out += L"\\t";
        } else if (c < 0x20) {
            wchar_t buf[8];
            swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Same escaping, UTF-8 encoded — for narrow JSON consumed by the
// enrollment UI.
inline std::string JsonEscapeUtf8(const std::wstring& s) {
    return WstrToUtf8(JsonEscape(s));
}

} // namespace facelogin
