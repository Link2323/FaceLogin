#include "credential_store.h"

#include "dpapi_util.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kFileMagic = 0x474F4C46;
int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

std::vector<float> Basis(size_t index, size_t dimension = 512) {
    std::vector<float> value(dimension, 0.0f);
    value[index] = 1.0f;
    return value;
}

std::vector<float> Midpoint(size_t first, size_t second) {
    std::vector<float> value(512, 0.0f);
    const float component = 1.0f / std::sqrt(2.0f);
    value[first] = component;
    value[second] = component;
    return value;
}

std::wstring MakeTempRoot() {
    wchar_t temp[MAX_PATH] = {};
    if (!GetTempPathW(ARRAYSIZE(temp), temp)) return {};
    const std::wstring root = std::wstring(temp) + L"FaceLogin-CredentialStoreTest-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    return CreateDirectoryW(root.c_str(), nullptr) ? root : std::wstring{};
}

void CleanupTempRoot(const std::wstring& root) {
    if (root.empty()) return;
    const std::wstring data = root + L"\\data";
    DeleteFileW((data + L"\\users.dat").c_str());
    RemoveDirectoryW(data.c_str());
    RemoveDirectoryW(root.c_str());
}

template <typename T>
void Write(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WriteText(std::ofstream& file, const std::wstring& value) {
    const uint32_t length = static_cast<uint32_t>(value.size());
    Write(file, length);
    if (length != 0) {
        file.write(reinterpret_cast<const char*>(value.data()),
                   static_cast<std::streamsize>(length * sizeof(wchar_t)));
    }
}

void WriteUnsupportedDatabase(const std::wstring& root, uint32_t version) {
    CreateDirectoryW((root + L"\\data").c_str(), nullptr);
    std::ofstream file(root + L"\\data\\users.dat", std::ios::binary | std::ios::trunc);
    const uint32_t count = 0;
    Write(file, kFileMagic);
    Write(file, version);
    Write(file, count);
}

void WriteValidV4Record(std::ofstream& file, const std::wstring& username,
                        const std::wstring& sid) {
    WriteText(file, username);
    WriteText(file, L"");
    WriteText(file, sid);
    const uint32_t passwordLength = 1;
    Write(file, passwordLength);
    const uint8_t passwordless = facelogin::kPasswordlessSentinelByte;
    Write(file, passwordless);
    const uint32_t faceCount = 1;
    const uint32_t faceId = 1;
    const uint32_t embeddingLength = 512;
    Write(file, faceCount);
    Write(file, faceId);
    WriteText(file, L"face");
    Write(file, embeddingLength);
    const auto embedding = Basis(0);
    file.write(reinterpret_cast<const char*>(embedding.data()),
               static_cast<std::streamsize>(embedding.size() * sizeof(float)));
}

void TestIdentityMatching() {
    facelogin::CredentialStore store;
    const auto password = facelogin::DpapiUtil::Protect(L"test-password");
    Check(!password.empty(), "DPAPI creates a machine-bound test credential");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(0)), "first Alice face is added");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(1)), "second Alice face is added");
    Check(store.AddFace(L"bob", L"bob@example.test", L"SID-B", password,
                        Basis(2)), "Bob face is added");

    const auto aliceProbe = Basis(1);
    auto identity = store.FindBestIdentity(aliceProbe.data(), aliceProbe.size(), 0.80f);
    Check(identity && identity->sid == L"SID-A",
          "identity-only match chooses the closest account without a credential field");

    const auto ambiguous = Midpoint(1, 2);
    Check(!store.FindBestIdentity(ambiguous.data(), ambiguous.size(), 1.00f),
          "best/second account ratio rejects an ambiguous probe");
    const auto legacyProbe = Basis(0, 128);
    Check(!store.FindBestIdentity(legacyProbe.data(), legacyProbe.size(), 0.80f),
          "different embedding dimensions are never compared");

    auto credential = store.LoadCredentialForSid(L"SID-A", identity ? identity->distance : 0.0f);
    Check(credential && credential->password == L"test-password" &&
          credential->sid == L"SID-A" && !credential->passwordless,
          "credential is decrypted only by final SID lookup");
    Check(!store.LoadCredentialForSid(L""), "empty authorized SID fails closed");
    Check(!store.LoadCredentialForSid(L"SID-MISSING"),
          "unknown authorized SID fails closed");

    Check(store.DeleteUserBySid(L"SID-A"), "authorized record can be removed");
    Check(!store.LoadCredentialForSid(L"SID-A"),
          "record disappearance between match and release fails closed");
}

void TestPasswordlessAndThresholds() {
    facelogin::CredentialStore store;
    const std::vector<uint8_t> sentinel{facelogin::kPasswordlessSentinelByte};
    const auto embedding = Basis(0);
    Check(store.AddFace(L"passwordless", L"", L"SID-P", sentinel, embedding),
          "passwordless record is added");
    const auto identity = store.FindBestIdentity(embedding.data(), embedding.size(), 0.80f);
    Check(identity && identity->passwordless,
          "identity-only result marks passwordless records before release");
    const auto credential = store.LoadCredentialForSid(L"SID-P");
    Check(credential && credential->passwordless && credential->password.empty(),
          "passwordless credential never materializes plaintext");

    Check(std::fabs(facelogin::EmbeddingThresholdForDim(0.80f, 512) - 0.80f) < 0.0001f,
          "calibrated 512-D threshold is preserved");
    Check(std::fabs(facelogin::EmbeddingThresholdForDim(0.20f, 512) - 0.80f) < 0.0001f,
          "unsafe-tight 512-D threshold returns safe default");
    Check(std::fabs(facelogin::EmbeddingThresholdForDim(1.20f, 512) - 1.00f) < 0.0001f,
          "unsafe-loose 512-D threshold is clamped");
}

void TestV4RoundTripAndReload() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "temporary credential-store directory is created");
    if (root.empty()) return;

    facelogin::CredentialStore writer;
    writer.SetDataDir(root);
    const auto encrypted = facelogin::DpapiUtil::Protect(L"round-trip-password");
    Check(writer.AddFace(L"roundtrip", L"roundtrip@example.test", L"SID-R",
                         encrypted, Basis(0), L"front"),
          "round-trip record is added");
    Check(writer.SaveDatabase(), "V4 database saves");

    facelogin::CredentialStore reader;
    reader.SetDataDir(root);
    Check(reader.LoadDatabase() && reader.GetUserCount() == 1 &&
          reader.GetFaceCount(L"SID-R") == 1,
          "V4 database reload preserves account and face");
    const auto credential = reader.LoadCredentialForSid(L"SID-R");
    Check(credential && credential->password == L"round-trip-password",
          "V4 reload preserves decryptable credential");

    // Replace the file with a valid empty database. A previously authorized
    // SID must disappear immediately after the reload.
    {
        std::ofstream file(root + L"\\data\\users.dat",
                           std::ios::binary | std::ios::trunc);
        const uint32_t version = 4;
        const uint32_t count = 0;
        Write(file, kFileMagic);
        Write(file, version);
        Write(file, count);
    }
    Check(reader.LoadDatabase() && reader.GetUserCount() == 0,
          "valid empty database atomically clears old records");
    Check(!reader.LoadCredentialForSid(L"SID-R"),
          "database reload disappearance fails closed");
    CleanupTempRoot(root);
}

void TestUnsupportedVersionsAreRejected() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "unsupported-version fixture directory is created");
    if (root.empty()) return;

    for (uint32_t version = 1; version <= 3; ++version) {
        WriteUnsupportedDatabase(root, version);
        facelogin::CredentialStore store;
        store.SetDataDir(root);
        Check(!store.LoadDatabase() && store.GetUserCount() == 0,
              "non-V4 database version is rejected");
    }
    CleanupTempRoot(root);
}

void TestMalformedReloadIsTransactional() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "malformed fixture directory is created");
    if (root.empty()) return;
    CreateDirectoryW((root + L"\\data").c_str(), nullptr);

    // Two records are declared. The first is complete; the second is
    // truncated after its username length. No prefix may become visible.
    {
        std::ofstream file(root + L"\\data\\users.dat",
                           std::ios::binary | std::ios::trunc);
        const uint32_t version = 4;
        const uint32_t count = 2;
        Write(file, kFileMagic);
        Write(file, version);
        Write(file, count);
        WriteValidV4Record(file, L"first", L"SID-FIRST");
        const uint32_t secondNameLength = 8;
        Write(file, secondNameLength);
    }
    facelogin::CredentialStore store;
    store.SetDataDir(root);
    Check(!store.LoadDatabase(), "truncated database reload is rejected");
    Check(store.GetUserCount() == 0,
          "rejected reload exposes no partially parsed identities");

    // The header count itself is bounded before any record allocation.
    {
        std::ofstream file(root + L"\\data\\users.dat",
                           std::ios::binary | std::ios::trunc);
        const uint32_t version = 4;
        const uint32_t count = static_cast<uint32_t>(facelogin::kMaxUsers + 1);
        Write(file, kFileMagic);
        Write(file, version);
        Write(file, count);
    }
    Check(!store.LoadDatabase() && store.GetUserCount() == 0,
          "oversized database user count fails closed");
    CleanupTempRoot(root);
}

} // namespace

int wmain() {
    TestIdentityMatching();
    TestPasswordlessAndThresholds();
    TestV4RoundTripAndReload();
    TestUnsupportedVersionsAreRejected();
    TestMalformedReloadIsTransactional();
    if (g_failures != 0) {
        std::fprintf(stderr, "CredentialStoreTest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("CredentialStoreTest: all checks passed");
    return 0;
}
