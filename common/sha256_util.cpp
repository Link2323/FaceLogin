#include "sha256_util.h"

#include "logger.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace facelogin {

namespace {

// Case-insensitive ASCII hex string comparison. The canonical manifest ships
// lowercase, but we tolerate either case so a future manifest edit can't break
// verification purely on casing.
bool HexEqualIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

}  // namespace

std::optional<std::string> FileSha256(const std::filesystem::path& path) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    const auto cleanup = [&]() {
        if (hash != 0) CryptDestroyHash(hash);
        if (provider != 0) CryptReleaseContext(provider, 0);
    };

    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        cleanup();
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        cleanup();
        return std::nullopt;
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead > 0 &&
            !CryptHashData(hash, buffer.data(), static_cast<DWORD>(bytesRead), 0)) {
            cleanup();
            return std::nullopt;
        }
    }
    if (input.bad()) {
        cleanup();
        return std::nullopt;
    }

    DWORD digestSize = 0;
    DWORD digestSizeLength = sizeof(digestSize);
    if (!CryptGetHashParam(hash, HP_HASHSIZE,
                           reinterpret_cast<BYTE*>(&digestSize),
                           &digestSizeLength, 0)) {
        cleanup();
        return std::nullopt;
    }
    std::vector<BYTE> digest(digestSize);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestSize, 0)) {
        cleanup();
        return std::nullopt;
    }

    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const BYTE value : digest) {
        encoded << std::setw(2) << static_cast<unsigned int>(value);
    }
    cleanup();
    return encoded.str();
}

bool VerifyModelIntegrity(const std::filesystem::path& path,
                          const std::string& expectedHex,
                          const wchar_t* modelLabel) {
    auto actual = FileSha256(path);
    if (!actual) {
        FACELOGIN_ERROR(L"Model integrity check failed: %s — unable to hash file "
                        L"(missing or unreadable); expected SHA-256 %hs",
                        modelLabel, expectedHex.c_str());
        return false;
    }
    if (!HexEqualIgnoreCase(*actual, expectedHex)) {
        FACELOGIN_ERROR(L"Model integrity check failed: %s — SHA-256 mismatch. "
                        L"Expected %hs, got %hs. The model may have been tampered "
                        L"with or corrupted; treating as fail-closed.",
                        modelLabel, expectedHex.c_str(), actual->c_str());
        return false;
    }
    return true;
}

}  // namespace facelogin
