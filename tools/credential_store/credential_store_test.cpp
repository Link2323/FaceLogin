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
    const uint32_t passwordLength = 2;
    Write(file, passwordLength);
    const uint8_t encryptedPassword[] = {0x01, 0x02};
    file.write(reinterpret_cast<const char*>(encryptedPassword), sizeof(encryptedPassword));
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
    Check(identity && identity->faceId == 2 && identity->faceLabel == L"脸2",
          "identity match reports which stored face won (progressive learning)");

    const auto ambiguous = Midpoint(1, 2);
    float ratioMissDistance = -1.0f;
    Check(!store.FindBestIdentity(ambiguous.data(), ambiguous.size(), 1.00f,
                                  &ratioMissDistance),
          "best/second account ratio rejects an ambiguous probe");
    Check(ratioMissDistance > 0.70f && ratioMissDistance < 0.80f,
          "outBestDistance reports the closest distance on ratio rejection");
    const auto legacyProbe = Basis(0, 128);
    float legacyMissDistance = -1.0f;
    Check(!store.FindBestIdentity(legacyProbe.data(), legacyProbe.size(), 0.80f,
                                  &legacyMissDistance),
          "different embedding dimensions are never compared");
    Check(legacyMissDistance < 0.0f,
          "outBestDistance is untouched when no embedding is comparable");
    const auto strangerProbe = Basis(3);
    float strangerMissDistance = -1.0f;
    Check(!store.FindBestIdentity(strangerProbe.data(), strangerProbe.size(), 0.80f,
                                  &strangerMissDistance),
          "probe far from every enrollment is rejected");
    Check(std::fabs(strangerMissDistance - std::sqrt(2.0f)) < 1e-4f,
          "outBestDistance reports how far off the failed probe was");

    auto credential = store.LoadCredentialForSid(L"SID-A", identity ? identity->distance : 0.0f);
    Check(credential && credential->password == L"test-password" &&
          credential->sid == L"SID-A",
          "credential is decrypted only by final SID lookup");
    Check(!store.LoadCredentialForSid(L""), "empty authorized SID fails closed");
    Check(!store.LoadCredentialForSid(L"SID-MISSING"),
          "unknown authorized SID fails closed");

    Check(store.DeleteUserBySid(L"SID-A"), "authorized record can be removed");
    Check(!store.LoadCredentialForSid(L"SID-A"),
          "record disappearance between match and release fails closed");
}

void TestThresholds() {
    Check(std::fabs(facelogin::EmbeddingThresholdForDim(0.80f, 512) - 0.80f) < 0.0001f,
          "calibrated 512-D threshold is preserved");
    Check(std::fabs(facelogin::EmbeddingThresholdForDim(0.20f, 512) - 0.80f) < 0.0001f,
          "unsafe-tight 512-D threshold returns safe default");
    Check(std::fabs(facelogin::EmbeddingThresholdForDim(1.20f, 512) - 1.00f) < 0.0001f,
          "unsafe-loose 512-D threshold is clamped");
}

void TestPasswordBlobValidation() {
    facelogin::CredentialStore store;
    const auto embedding = Basis(0);
    Check(!store.AddFace(L"invalid", L"", L"SID-I", {}, embedding),
          "empty password blob is rejected");
    Check(!store.AddFace(L"invalid", L"", L"SID-I", {0x00}, embedding),
          "single-byte legacy sentinel is rejected");
}

// Unit vector tilted `perturb` away from a basis axis (still ~orthogonal to
// every other axis — distance to Basis(index) ≈ atan(perturb)).
std::vector<float> NearBasis(size_t index, float perturb) {
    std::vector<float> value(512, 0.0f);
    value[index] = 1.0f;
    value[(index + 7) % 512] = perturb;
    float norm = 0.0f;
    for (float v : value) norm += v * v;
    norm = std::sqrt(norm);
    for (float& v : value) v /= norm;
    return value;
}

float VectorDistance(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        const float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

void TestUpdateTemplateFace() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "temp root created for template update tests");
    if (root.empty()) return;

    facelogin::CredentialStore store;
    store.SetDataDir(root);
    const auto password = facelogin::DpapiUtil::Protect(L"test-password");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", password,
                        Basis(0), L"正面"),
          "front face is added for update tests");
    Check(store.AddFace(L"alice", L"alice@example.test", L"SID-A", {},
                        Basis(1), L"左转"),
          "left face is added for update tests");

    const auto inCone = NearBasis(0, 0.10f);   // ≈0.10 from Basis(0), ≈1.40 from Basis(1)
    Check(store.UpdateTemplateFace(L"SID-A", 1, inCone),
          "same-pose EMA update is accepted");

    // Pair-spacing observation (2026-09-04): replacing face #1 with a vector
    // only ~0.10 from face #2 collapses the pair below
    // kCrossAngleSentinelDist — the update COMMITS anyway (a rejecting
    // sentinel wedged the whole learning channel on legacy near-pair
    // accounts; convergence is fail-safe coverage loss, docs red line 3).
    const auto nearFace2 = NearBasis(1, 0.10f);
    Check(store.UpdateTemplateFace(L"SID-A", 1, nearFace2),
          "converging update commits (pair distance is observation-only)");
    {
        const auto& users = store.GetUsers();
        Check(users.size() == 1 && users[0].faces.size() == 2 &&
                  VectorDistance(users[0].faces[0].embedding, nearFace2) < 1e-5f,
              "observation-only update actually replaces the template");
    }

    Check(!store.UpdateTemplateFace(L"SID-MISSING", 1, inCone),
          "unknown SID update is rejected");
    Check(!store.UpdateTemplateFace(L"SID-A", 9, inCone),
          "unknown face id update is rejected");
    Check(!store.UpdateTemplateFace(L"SID-A", 1, std::vector<float>(256, 0.5f)),
          "dimension-mismatched update is rejected");

    // Persisted: a fresh store reloads the updated template with ids/labels
    // and the password intact (learning must never corrupt the credential).
    Check(store.SaveDatabase(), "updated database saves");
    {
        facelogin::CredentialStore reloaded;
        reloaded.SetDataDir(root);
        Check(reloaded.LoadDatabase(), "updated database reloads");
        const auto& users = reloaded.GetUsers();
        Check(users.size() == 1 && users[0].faces.size() == 2 &&
                  users[0].faces[0].label == L"正面" &&
                  users[0].faces[1].label == L"左转" &&
                  VectorDistance(users[0].faces[0].embedding, nearFace2) < 1e-5f,
              "updated template round-trips with labels and slot order");
        auto credential = reloaded.LoadCredentialForSid(L"SID-A", 0.1f);
        Check(credential && credential->password == L"test-password",
              "password survives template updates");
    }
    CleanupTempRoot(root);
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

void TestV5NominalAnglesAndUpgradeRead() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "temp root created for V5 nominal-angle tests");
    if (root.empty()) return;

    // V5 round trip: nominal angles persist per face.
    {
        facelogin::CredentialStore writer;
        writer.SetDataDir(root);
        const auto encrypted = facelogin::DpapiUtil::Protect(L"v5-password");
        Check(writer.AddFace(L"v5", L"", L"SID-V5", encrypted, Basis(0),
                             L"正面", nullptr, 0.0f, 0.0f),
              "front face with nominal angles is added");
        Check(writer.AddFace(L"v5", L"", L"SID-V5", {}, Basis(1),
                             L"左转", nullptr, 30.0f, 0.0f),
              "left face with nominal yaw +30 is added");
        Check(writer.SaveDatabase(), "V5 database saves");
        facelogin::CredentialStore reader;
        reader.SetDataDir(root);
        Check(reader.LoadDatabase(), "V5 database reloads");
        const auto& users = reader.GetUsers();
        Check(users.size() == 1 && users[0].faces.size() == 2 &&
                  users[0].faces[0].nominalYaw == 0.0f &&
                  users[0].faces[1].nominalYaw == 30.0f &&
                  users[0].faces[0].nominalPitch == 0.0f &&
                  users[0].faces[1].nominalPitch == 0.0f,
              "V5 nominal angles round-trip per face");
        auto credential = reader.LoadCredentialForSid(L"SID-V5");
        Check(credential && credential->password == L"v5-password",
              "V5 reload preserves decryptable credential");

        // Landing-clarity input: the winning account's second-nearest face
        // is exposed on IdentityMatch; a probe near face #1 reports face #2's
        // distance as the runner-up.
        const auto probe = NearBasis(0, 0.05f);
        auto identity = reader.FindBestIdentity(probe.data(), probe.size(), 0.80f);
        Check(identity && identity->faceId == 1 &&
                  identity->secondFaceDistance > 1.30f,
              "identity match reports the winning account's runner-up face");
        // Single-face account: negative runner-up = clarity gate passes.
        facelogin::CredentialStore single;
        single.SetDataDir(root);
        const auto encrypted2 = facelogin::DpapiUtil::Protect(L"v5b");
        Check(single.AddFace(L"solo", L"", L"SID-SOLO", encrypted2, Basis(4)),
              "single-face account is added");
        auto solo = single.FindBestIdentity(Basis(4).data(), 512, 0.80f);
        Check(solo && solo->secondFaceDistance < 0.0f,
              "single-face account reports no runner-up face");
    }

    // V4 upgrade read: a legacy file loads with invalid nominal angles.
    {
        {
            std::ofstream file(root + L"\\data\\users.dat",
                               std::ios::binary | std::ios::trunc);
            const uint32_t version = 4;
            const uint32_t count = 1;
            Write(file, kFileMagic);
            Write(file, version);
            Write(file, count);
            WriteValidV4Record(file, L"legacy", L"SID-LEGACY");
        }  // close+flush the fixture before the reader opens it
        facelogin::CredentialStore reader;
        reader.SetDataDir(root);
        Check(reader.LoadDatabase(), "V4 database still loads (V5 reader)");
        const auto& users = reader.GetUsers();
        Check(users.size() == 1 && users[0].faces.size() == 1 &&
                  users[0].faces[0].nominalYaw == facelogin::kNominalAngleInvalid &&
                  users[0].faces[0].nominalPitch == facelogin::kNominalAngleInvalid,
              "V4 faces load with invalid nominal angles");
        // Saving upgrades the file to V5 with the sentinel round-tripped.
        Check(reader.SaveDatabase(), "upgraded database saves as V5");
        facelogin::CredentialStore rereader;
        rereader.SetDataDir(root);
        Check(rereader.LoadDatabase(), "upgraded V5 file reloads");
        const auto& upgraded = rereader.GetUsers();
        Check(upgraded.size() == 1 && upgraded[0].faces.size() == 1 &&
                  upgraded[0].faces[0].nominalYaw == facelogin::kNominalAngleInvalid,
              "V5 round-trips the invalid-angle sentinel (cone stays off)");
    }
    CleanupTempRoot(root);
}

void TestUnsupportedVersionsAreRejected() {
    const std::wstring root = MakeTempRoot();
    Check(!root.empty(), "unsupported-version fixture directory is created");
    if (root.empty()) return;

    for (uint32_t version : {1u, 2u, 3u, 6u}) {
        WriteUnsupportedDatabase(root, version);
        facelogin::CredentialStore store;
        store.SetDataDir(root);
        Check(!store.LoadDatabase() && store.GetUserCount() == 0,
              "unsupported database version is rejected");
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
    TestThresholds();
    TestPasswordBlobValidation();
    TestV4RoundTripAndReload();
    TestUpdateTemplateFace();
    TestV5NominalAnglesAndUpgradeRead();
    TestUnsupportedVersionsAreRejected();
    TestMalformedReloadIsTransactional();
    if (g_failures != 0) {
        std::fprintf(stderr, "CredentialStoreTest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("CredentialStoreTest: all checks passed");
    return 0;
}
