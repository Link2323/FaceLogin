#include "credential_store.h"
#include "../common/logger.h"
#include "../common/dpapi_util.h"
#include <shlobj.h>
#include <fstream>
#include <algorithm>

namespace facelogin {

static constexpr uint32_t FILE_MAGIC = 0x474F4C46; // "FLOG" in little-endian
static constexpr uint32_t FILE_VERSION = 5;        // written; reads accept 4/5

// Helpers for V4/V5 serialization
namespace {

// Default label for a face: L"脸N" where N = face id.
inline std::wstring DefaultFaceLabel(uint32_t id) {
    return L"脸" + std::to_wstring(id);  // 脸N
}

// Trim a face label for storage (empty → default). Keeps labels sane.
inline std::wstring NormalizeFaceLabel(const std::wstring& label, uint32_t id) {
    if (label.empty()) return DefaultFaceLabel(id);
    return label;
}

} // namespace

std::wstring CredentialStore::GetDataDir() const {
    if (!m_dataDir.empty()) return m_dataDir;

    // Default: %PROGRAMDATA%\FaceLogin
    wchar_t programData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        return std::wstring(programData) + L"\\FaceLogin";
    }
    return L"C:\\ProgramData\\FaceLogin";
}

bool CredentialStore::LoadDatabase() {
    std::wstring path = GetDataDir() + L"\\data\\users.dat";
    // Reload is transactional and fail-closed. Never expose a prefix of a
    // malformed database: authentication must see either the complete newly
    // validated file or no identities at all.
    m_users.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        FACELOGIN_INFO(L"No existing database at %s (this is normal on first run)", path.c_str());
        return true; // Empty database is valid
    }

    // Read header
    uint32_t magic = 0, version = 0, count = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (magic != FILE_MAGIC) {
        FACELOGIN_ERROR(L"Invalid database file (bad magic: 0x%08X)", magic);
        return false;
    }
    if (version != 4 && version != 5) {
        FACELOGIN_ERROR(L"Unsupported database version: %u", version);
        return false;
    }
    if (count > kMaxUsers) {
        FACELOGIN_ERROR(L"Invalid database user count: %u", count);
        return false;
    }

    FACELOGIN_INFO(L"Loading %u user record(s) from database (v%u)", count, version);
    std::vector<UserRecord> loadedUsers;
    loadedUsers.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        UserRecord rec = {};

        // Username
        uint32_t nameLen = 0;
        file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (nameLen == 0 || nameLen > 256) {
            FACELOGIN_ERROR(L"Invalid username length: %u", nameLen);
            return false;
        }
        std::vector<wchar_t> nameBuf(nameLen + 1, 0);
        file.read(reinterpret_cast<char*>(nameBuf.data()), nameLen * sizeof(wchar_t));
        rec.username = nameBuf.data();

        // UPN
        uint32_t upnLen = 0;
        file.read(reinterpret_cast<char*>(&upnLen), sizeof(upnLen));
        if (upnLen > 256) {
            FACELOGIN_ERROR(L"Invalid UPN length: %u", upnLen);
            return false;
        }
        if (upnLen > 0) {
            std::vector<wchar_t> upnBuf(upnLen + 1, 0);
            file.read(reinterpret_cast<char*>(upnBuf.data()), upnLen * sizeof(wchar_t));
            rec.upn = upnBuf.data();
        }

        // SID
        uint32_t sidLen = 0;
        file.read(reinterpret_cast<char*>(&sidLen), sizeof(sidLen));
        if (sidLen > 512) {
            FACELOGIN_ERROR(L"Invalid SID length: %u", sidLen);
            return false;
        }
        if (sidLen > 0) {
            std::vector<wchar_t> sidBuf(sidLen + 1, 0);
            file.read(reinterpret_cast<char*>(sidBuf.data()), sidLen * sizeof(wchar_t));
            rec.sid = sidBuf.data();
        }

        // Password
        uint32_t passLen = 0;
        file.read(reinterpret_cast<char*>(&passLen), sizeof(passLen));
        if (passLen < 2 || passLen > 4096) {
            FACELOGIN_ERROR(L"Invalid password length: %u", passLen);
            return false;
        }
        rec.encryptedPassword.resize(passLen);
        if (passLen > 0) {
            file.read(reinterpret_cast<char*>(rec.encryptedPassword.data()), passLen);
        }

        // V4: one or more faces, each with id/label/embedding.
        uint32_t faceCount = 0;
        file.read(reinterpret_cast<char*>(&faceCount), sizeof(faceCount));
        // Writer is capped at kMaxFacesPerUser; read-side is lenient to
        // avoid killing the whole DB on a slightly-over spec file.
        if (faceCount < 1 || faceCount > 16) {
            FACELOGIN_ERROR(L"Invalid face count: %u", faceCount);
            return false;
        }
        rec.faces.reserve(faceCount);
        for (uint32_t f = 0; f < faceCount; f++) {
            FaceRecord face;
            file.read(reinterpret_cast<char*>(&face.id), sizeof(face.id));
            if (face.id < 1) {
                FACELOGIN_ERROR(L"Invalid face id: %u", face.id);
                return false;
            }
            uint32_t labelLen = 0;
            file.read(reinterpret_cast<char*>(&labelLen), sizeof(labelLen));
            if (labelLen > 64) {
                FACELOGIN_ERROR(L"Invalid face label length: %u", labelLen);
                return false;
            }
            if (labelLen > 0) {
                std::vector<wchar_t> labelBuf(labelLen + 1, 0);
                file.read(reinterpret_cast<char*>(labelBuf.data()),
                          labelLen * sizeof(wchar_t));
                face.label = labelBuf.data();
            }
            uint32_t embLen = 0;
            file.read(reinterpret_cast<char*>(&embLen), sizeof(embLen));
            if (embLen < 64 || embLen > 4096) {
                FACELOGIN_ERROR(L"Invalid embedding length: %u", embLen);
                return false;
            }
            face.embedding.resize(embLen);
            file.read(reinterpret_cast<char*>(face.embedding.data()),
                      embLen * sizeof(float));
            // V5 appends the slot's nominal pose angles; V4 records keep the
            // invalid sentinel until re-enrollment. The sentinel itself is a
            // legal stored value (legacy records round-trip it), so only
            // non-finite data is corruption.
            if (version >= 5) {
                file.read(reinterpret_cast<char*>(&face.nominalYaw),
                          sizeof(face.nominalYaw));
                file.read(reinterpret_cast<char*>(&face.nominalPitch),
                          sizeof(face.nominalPitch));
                if (!std::isfinite(face.nominalYaw) ||
                    !std::isfinite(face.nominalPitch)) {
                    FACELOGIN_ERROR(L"Invalid nominal angle for face %u", face.id);
                    return false;
                }
            }
            rec.faces.push_back(std::move(face));
        }

        if (file.good()) {
            loadedUsers.push_back(std::move(rec));
        } else {
            FACELOGIN_ERROR(L"Failed to read record %u", i);
            return false;
        }
    }

    m_users = std::move(loadedUsers);
    FACELOGIN_INFO(L"Loaded %zu user(s) successfully", m_users.size());
    return true;
}

bool CredentialStore::SaveDatabase() {
    std::wstring dataDir = GetDataDir() + L"\\data";
    CreateDirectoryW(dataDir.c_str(), nullptr);
    if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != 0) {
        FACELOGIN_ERROR(L"Failed to create data directory: %s", dataDir.c_str());
        return false;
    }

    std::wstring path = GetDataDir() + L"\\data\\users.dat";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        FACELOGIN_ERROR(L"Failed to open database for writing: %s", path.c_str());
        return false;
    }

    // Write header
    uint32_t magic = FILE_MAGIC;
    uint32_t version = FILE_VERSION;
    // Skip accounts with zero faces defensively — DeleteFace removes a whole
    // account when its last face is deleted, so this should never occur.
    uint32_t count = 0;
    for (const auto& rec : m_users) {
        if (!rec.faces.empty()) count++;
    }
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& rec : m_users) {
        if (rec.faces.empty()) continue;  // defensive: never persist 0-face account

        // Username
        uint32_t nameLen = static_cast<uint32_t>(rec.username.size());
        file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        file.write(reinterpret_cast<const char*>(rec.username.c_str()),
                   nameLen * sizeof(wchar_t));

        // UPN
        uint32_t upnLen = static_cast<uint32_t>(rec.upn.size());
        file.write(reinterpret_cast<const char*>(&upnLen), sizeof(upnLen));
        if (upnLen > 0) {
            file.write(reinterpret_cast<const char*>(rec.upn.c_str()),
                       upnLen * sizeof(wchar_t));
        }

        // SID
        uint32_t sidLen = static_cast<uint32_t>(rec.sid.size());
        file.write(reinterpret_cast<const char*>(&sidLen), sizeof(sidLen));
        if (sidLen > 0) {
            file.write(reinterpret_cast<const char*>(rec.sid.c_str()),
                       sidLen * sizeof(wchar_t));
        }

        // Password
        uint32_t passLen = static_cast<uint32_t>(rec.encryptedPassword.size());
        file.write(reinterpret_cast<const char*>(&passLen), sizeof(passLen));
        file.write(reinterpret_cast<const char*>(rec.encryptedPassword.data()), passLen);

        // Faces (V4)
        uint32_t faceCount = static_cast<uint32_t>(rec.faces.size());
        file.write(reinterpret_cast<const char*>(&faceCount), sizeof(faceCount));
        for (const auto& face : rec.faces) {
            uint32_t faceId = face.id;
            file.write(reinterpret_cast<const char*>(&faceId), sizeof(faceId));

            uint32_t labelLen = static_cast<uint32_t>(face.label.size());
            file.write(reinterpret_cast<const char*>(&labelLen), sizeof(labelLen));
            if (labelLen > 0) {
                file.write(reinterpret_cast<const char*>(face.label.c_str()),
                           labelLen * sizeof(wchar_t));
            }

            uint32_t embLen = static_cast<uint32_t>(face.embedding.size());
            file.write(reinterpret_cast<const char*>(&embLen), sizeof(embLen));
            if (embLen > 0) {
                file.write(reinterpret_cast<const char*>(face.embedding.data()),
                           embLen * sizeof(float));
            }

            // V5: nominal pose angles of the slot (sentinel round-trips).
            file.write(reinterpret_cast<const char*>(&face.nominalYaw),
                       sizeof(face.nominalYaw));
            file.write(reinterpret_cast<const char*>(&face.nominalPitch),
                       sizeof(face.nominalPitch));
        }
    }

    file.close();
    FACELOGIN_INFO(L"Saved %zu user(s) to database (v4)", count);
    return true;
}

size_t CredentialStore::FindUserIndex(const std::wstring& sid,
                                      const std::wstring& upn,
                                      const std::wstring& username) const {
    if (!sid.empty()) {
        auto it = std::find_if(m_users.begin(), m_users.end(),
            [&sid](const UserRecord& r) { return r.sid == sid; });
        if (it != m_users.end()) return static_cast<size_t>(it - m_users.begin());
    }
    if (!upn.empty()) {
        auto it = std::find_if(m_users.begin(), m_users.end(),
            [&upn](const UserRecord& r) { return r.upn == upn; });
        if (it != m_users.end()) return static_cast<size_t>(it - m_users.begin());
    }
    if (!username.empty()) {
        auto it = std::find_if(m_users.begin(), m_users.end(),
            [&username](const UserRecord& r) { return r.username == username; });
        if (it != m_users.end()) return static_cast<size_t>(it - m_users.begin());
    }
    return m_users.size();
}

bool CredentialStore::AddFace(const std::wstring& username,
                              const std::wstring& upn,
                              const std::wstring& sid,
                              const std::vector<uint8_t>& encryptedPassword,
                              const std::vector<float>& embedding,
                              const std::wstring& label,
                              uint32_t* outFaceId,
                              float nominalYaw,
                              float nominalPitch) {
    size_t idx = FindUserIndex(sid, upn, username);

    if (idx < m_users.size()) {
        // Account exists → append a face, never touch stored password/faces.
        UserRecord& rec = m_users[idx];
        if (rec.faces.size() >= kMaxFacesPerUser) {
            FACELOGIN_WARN(L"AddFace rejected: %s already has %zu faces (max %zu)",
                           username.c_str(), rec.faces.size(), kMaxFacesPerUser);
            return false;
        }
        // Reuse the smallest free id so ids stay compact (1..N) after
        // deletions — otherwise deleting #2 and re-adding leaves gaps like
        // #1,#3,#4. (Multi-angle replace already restarts ids at 1 after
        // ClearFacesForAccount, so "never reuse" was never a real invariant.)
        uint32_t newId = 1;
        bool taken = true;
        while (taken) {
            taken = false;
            for (const auto& f : rec.faces) {
                if (f.id == newId) { taken = true; newId++; break; }
            }
        }
        FaceRecord face;
        face.id = newId;
        face.label = NormalizeFaceLabel(label, newId);
        face.embedding = embedding;
        face.nominalYaw = nominalYaw;
        face.nominalPitch = nominalPitch;
        rec.faces.push_back(std::move(face));
        // Refresh display identity in case username/upn/sid changed.
        rec.username = username;
        rec.upn = upn;
        rec.sid = sid;
        if (outFaceId) *outFaceId = newId;
        FACELOGIN_INFO(L"Appended face #%u to %s (SID=%s, emb=%zu-D, total=%zu)",
                       newId, username.c_str(), sid.c_str(), embedding.size(),
                       rec.faces.size());
        return true;
    }

    // Account not found → create it with the given password and first face.
    if (encryptedPassword.size() < 2) {
        FACELOGIN_WARN(L"AddFace rejected: missing or invalid encrypted password for %s",
                       username.c_str());
        return false;
    }
    if (m_users.size() >= kMaxUsers) {
        FACELOGIN_WARN(L"AddFace rejected: database has %zu users (max %zu)",
                       m_users.size(), kMaxUsers);
        return false;
    }
    UserRecord rec;
    rec.username = username;
    rec.upn = upn;
    rec.sid = sid;
    rec.encryptedPassword = encryptedPassword;
    FaceRecord face;
    face.id = 1;
    face.label = NormalizeFaceLabel(label, 1);
    face.embedding = embedding;
    face.nominalYaw = nominalYaw;
    face.nominalPitch = nominalPitch;
    rec.faces.push_back(std::move(face));
    m_users.push_back(std::move(rec));
    if (outFaceId) *outFaceId = 1;
    FACELOGIN_INFO(L"Created user %s with first face (SID=%s, emb=%zu-D)",
                   username.c_str(), sid.c_str(), embedding.size());
    return true;
}

bool CredentialStore::UpdateAccountIdentity(size_t idx,
                                            const std::wstring& username,
                                            const std::wstring& upn,
                                            const std::wstring& sid,
                                            const std::vector<uint8_t>& encryptedPassword) {
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"UpdateAccountIdentity: index %zu out of range", idx);
        return false;
    }
    if (encryptedPassword.size() < 2) {
        FACELOGIN_WARN(L"UpdateAccountIdentity: missing or invalid encrypted password for %s",
                       username.c_str());
        return false;
    }

    UserRecord& rec = m_users[idx];
    if (!rec.faces.empty()) {
        // In-place identity + password refresh; faces untouched.
        rec.username = username;
        rec.upn      = upn;
        rec.sid      = sid;
        rec.encryptedPassword = encryptedPassword;
        FACELOGIN_INFO(L"Updated identity of '%s' → username=%s SID=%s UPN=%s (faces preserved)",
                       username.c_str(), username.c_str(), sid.c_str(), upn.c_str());
        return true;
    }

    // rec has no faces (defensive — SaveDatabase never persists 0-face
    // records, but an in-memory edge could exist). Instead of persisting an
    // empty record, merge its identity into the matching live record.
    size_t target = m_users.size();
    if (!upn.empty()) target = FindUserIndex(L"", upn, L"");
    if (target >= m_users.size() && !username.empty()) target = FindUserIndex(L"", L"", username);
    if (target < m_users.size()) {
        UserRecord& dst = m_users[target];
        dst.username = username;
        dst.upn      = upn;
        dst.sid      = sid;
        dst.encryptedPassword = encryptedPassword;
        m_users.erase(m_users.begin() + static_cast<ptrdiff_t>(idx));
        FACELOGIN_INFO(L"Merged identity of empty stale record into existing account %s",
                       username.c_str());
        return true;
    }
    return false;
}

bool CredentialStore::DeleteFace(const std::wstring& sid, uint32_t faceId) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"DeleteFace: account not found (SID=%s)", sid.c_str());
        return false;
    }
    UserRecord& rec = m_users[idx];
    auto it = std::remove_if(rec.faces.begin(), rec.faces.end(),
        [faceId](const FaceRecord& f) { return f.id == faceId; });
    if (it == rec.faces.end()) {
        FACELOGIN_WARN(L"DeleteFace: face #%u not found for %s", faceId, sid.c_str());
        return false;
    }
    rec.faces.erase(it, rec.faces.end());
    FACELOGIN_INFO(L"Deleted face #%u from %s (%zu remaining)",
                   faceId, rec.username.c_str(), rec.faces.size());
    if (rec.faces.empty()) {
        // Last face removed → drop the account entirely so the login tile
        // (which reads the record count) doesn't show a tile that can never
        // match. Re-enrollment goes through the first-time flow again.
        m_users.erase(m_users.begin() + static_cast<ptrdiff_t>(idx));
        FACELOGIN_INFO(L"Removed account %s (no faces remain)", rec.username.c_str());
    }
    return true;
}

bool CredentialStore::ClearAllFaces(const std::wstring& sid) {
    return DeleteUserBySid(sid);
}

bool CredentialStore::ClearFacesForAccount(const std::wstring& sid) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"ClearFacesForAccount: account not found (SID=%s)", sid.c_str());
        return false;
    }
    size_t removed = m_users[idx].faces.size();
    m_users[idx].faces.clear();
    FACELOGIN_INFO(L"Cleared %zu face(s) from %s (SID=%s), account identity preserved",
                   removed, m_users[idx].username.c_str(), sid.c_str());
    return true;
}

bool CredentialStore::DeleteUserBySid(const std::wstring& sid) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"DeleteUserBySid: account not found (SID=%s)", sid.c_str());
        return false;
    }
    std::wstring username = m_users[idx].username;
    m_users.erase(m_users.begin() + static_cast<ptrdiff_t>(idx));
    FACELOGIN_INFO(L"Deleted user %s (SID=%s)", username.c_str(), sid.c_str());
    return true;
}

bool CredentialStore::RenameFace(const std::wstring& sid, uint32_t faceId,
                                 const std::wstring& label) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) return false;
    UserRecord& rec = m_users[idx];
    for (auto& f : rec.faces) {
        if (f.id == faceId) {
            f.label = NormalizeFaceLabel(label, faceId);
            FACELOGIN_INFO(L"Renamed face #%u of %s → %s",
                           faceId, rec.username.c_str(), f.label.c_str());
            return true;
        }
    }
    return false;
}

size_t CredentialStore::GetFaceCount(const std::wstring& sid) const {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) return 0;
    return m_users[idx].faces.size();
}

std::optional<CredentialStore::IdentityMatch> CredentialStore::FindBestIdentity(
    const float probeEmbedding[], size_t probeDim, float threshold,
    float* outBestDistance) {

    if (m_users.empty() || probeDim == 0 || probeEmbedding == nullptr) {
        return std::nullopt;
    }

    // Account-level matching. Each account's closest face is its
    // representative distance; accounts are then compared against each other.
    // This keeps two faces of the SAME account from competing and inflating
    // the best/second-best ratio.
    float bestDist = 1e10f, secondBestDist = 1e10f;
    size_t bestIdx = m_users.size();
    uint32_t bestFaceId = 0;
    std::wstring bestFaceLabel;
    // Winning account's second-nearest face (landing-clarity gate input).
    float bestSecondFaceDist = -1.0f;
    size_t comparableAccounts = 0;

    for (size_t i = 0; i < m_users.size(); i++) {
        const auto& faces = m_users[i].faces;
        float accountBest = 1e10f, accountSecond = 1e10f;
        uint32_t accountBestFaceId = 0;
        const std::wstring* accountBestLabel = nullptr;

        for (const auto& face : faces) {
            // Skip stored embeddings that don't match the probe's dimensionality.
            // dlib (128-D) and InsightFace ONNX (512-D) embeddings live in
            // different metric spaces — comparing them would be meaningless.
            if (face.embedding.size() != probeDim) continue;

            float sum = 0.0f;
            for (size_t j = 0; j < probeDim; j++) {
                float diff = probeEmbedding[j] - face.embedding[j];
                sum += diff * diff;
            }
            float dist = std::sqrt(sum);

            if (dist < accountBest) {
                accountSecond = accountBest;
                accountBest = dist;
                accountBestFaceId = face.id;
                accountBestLabel = &face.label;
            } else if (dist < accountSecond) {
                accountSecond = dist;
            }
        }

        if (accountBest >= 1e9f) continue;  // no face with a comparable dimension
        comparableAccounts++;

        if (accountBest < bestDist) {
            secondBestDist = bestDist;
            bestDist = accountBest;
            bestIdx = i;
            bestFaceId = accountBestFaceId;
            bestFaceLabel = accountBestLabel ? *accountBestLabel : L"";
            bestSecondFaceDist =
                (accountSecond < 1e9f) ? accountSecond : -1.0f;
        } else if (accountBest < secondBestDist) {
            secondBestDist = accountBest;
        }
    }

    // No account with a comparable-dimensionality embedding.
    // (e.g. dlib 128-D probe against an ONNX 512-D enrollment — a config/data
    // mismatch. DEBUG level: fires on every frame and would spam the log.)
    if (bestIdx >= m_users.size()) {
        FACELOGIN_DEBUG(L"FindBestIdentity: no stored %zu-D embedding (accounts=%zu)",
                        probeDim, m_users.size());
        return std::nullopt;
    }

    // Publish the closest comparable distance before any rejection so
    // callers can log how far off a failed probe was.
    if (outBestDistance) *outBestDistance = bestDist;

    // The base threshold comes from config (default 0.80). For 512-D ONNX,
    // EmbeddingThresholdForDim honors it inside the calibrated band [0.70,
    // 1.00] and clamps outside values to that band — see credential_store.h.
    // For 128-D dlib it returns the base unchanged.
    // (DEBUG level: this runs on every frame and would spam the log.)
    float effThreshold = EmbeddingThresholdForDim(threshold, probeDim);
    if (effThreshold != threshold) {
        FACELOGIN_DEBUG(L"FindBestIdentity: dim=%zu → threshold %.3f scaled to %.3f",
                        probeDim, threshold, effThreshold);
    }

    // Reject if best match is not meaningfully better than second-best.
    // A ratio >= 0.75 means the probe is ambiguous between two accounts
    // (or between the real user and a noisy impostor).
    // Skip this check when only one account is comparable — there is no
    // second-best to compare against.
    if (comparableAccounts > 1 && secondBestDist < 1e9f) {
        float ratio = bestDist / secondBestDist;
        if (ratio >= 0.75f) {
            FACELOGIN_INFO(L"Match rejected: best/second-best ratio too high (%.3f/%.3f=%.3f)",
                          bestDist, secondBestDist, ratio);
            return std::nullopt;
        }
    }

    if (bestDist < effThreshold) {
        IdentityMatch best;
        best.distance = bestDist;
        best.upn = m_users[bestIdx].upn;
        best.sid = m_users[bestIdx].sid;
        best.username = m_users[bestIdx].username;
        best.faceId = bestFaceId;
        best.faceLabel = bestFaceLabel;
        best.secondFaceDistance = bestSecondFaceDist;
        return best;
    }

    return std::nullopt;
}

std::vector<CredentialStore::AccountFaceDistance>
CredentialStore::GetAccountFaceDistances(const std::wstring& sid,
                                         const float probeEmbedding[],
                                         size_t probeDim) const {
    std::vector<AccountFaceDistance> out;
    if (!probeEmbedding || probeDim == 0) return out;
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) return out;
    for (const auto& face : m_users[idx].faces) {
        // Same dimensionality rule as FindBestIdentity: a non-comparable
        // embedding belongs to a different metric space and has no distance.
        if (face.embedding.size() != probeDim) continue;
        float sum = 0.0f;
        for (size_t j = 0; j < probeDim; j++) {
            float diff = probeEmbedding[j] - face.embedding[j];
            sum += diff * diff;
        }
        AccountFaceDistance& d = out.emplace_back();
        d.faceId = face.id;
        d.label = face.label;
        d.nominalYaw = face.nominalYaw;
        d.nominalPitch = face.nominalPitch;
        d.distance = std::sqrt(sum);
    }
    return out;
}

bool CredentialStore::UpdateTemplateFace(const std::wstring& sid, uint32_t faceId,
                                         const std::vector<float>& newEmbedding) {
    size_t idx = FindUserIndex(sid, L"", L"");
    if (idx >= m_users.size()) {
        FACELOGIN_WARN(L"UpdateTemplateFace: account not found");
        return false;
    }
    auto& faces = m_users[idx].faces;
    FaceRecord* target = nullptr;
    for (auto& f : faces) {
        if (f.id == faceId) { target = &f; break; }
    }
    if (!target) {
        FACELOGIN_WARN(L"UpdateTemplateFace: face #%u not found", faceId);
        return false;
    }
    if (newEmbedding.size() != target->embedding.size()) {
        FACELOGIN_WARN(L"UpdateTemplateFace: dimension mismatch (%zu vs %zu)",
                       newEmbedding.size(), target->embedding.size());
        return false;
    }

    // Swap the candidate in unconditionally, then observe the account's
    // minimum template-pair distance (docs/progressive-learning-v2.md red
    // line 3, 2026-09-04: observation only — a rejecting sentinel's false
    // positives wedge the whole learning channel on legacy near-pair
    // accounts, and intra-account convergence only shrinks the acceptance
    // region, which is the fail-safe direction).
    target->embedding = newEmbedding;
    float minPairDist = -1.0f;
    uint32_t pairA = 0, pairB = 0;
    for (size_t i = 0; i < faces.size(); i++) {
        for (size_t j = i + 1; j < faces.size(); j++) {
            const auto& a = faces[i].embedding;
            const auto& b = faces[j].embedding;
            if (a.size() != b.size() || a.empty()) continue;
            float sum = 0.0f;
            for (size_t k = 0; k < a.size(); k++) {
                float diff = a[k] - b[k];
                sum += diff * diff;
            }
            float dist = std::sqrt(sum);
            if (minPairDist < 0.0f || dist < minPairDist) {
                minPairDist = dist;
                pairA = faces[i].id;
                pairB = faces[j].id;
            }
        }
    }
    if (minPairDist >= 0.0f) {
        if (minPairDist < kCrossAngleSentinelDist) {
            FACELOGIN_WARN(L"UpdateTemplateFace observation: faces #%u/#%u now "
                           L"%.3f apart (< %.2f) — slot coverage shrinking, learning "
                           L"continues (observation-only since 2026-09-04)",
                           pairA, pairB, minPairDist, kCrossAngleSentinelDist);
        } else {
            FACELOGIN_INFO(L"UpdateTemplateFace: min template-pair distance "
                           "#%u/#%u = %.3f", pairA, pairB, minPairDist);
        }
    }
    return true;
}

std::optional<CredentialStore::MatchResult> CredentialStore::LoadCredentialForSid(
    const std::wstring& sid, float distance) {
    if (sid.empty()) {
        FACELOGIN_ERROR(L"LoadCredentialForSid called with an empty SID");
        return std::nullopt;
    }

    const auto it = std::find_if(m_users.begin(), m_users.end(),
        [&sid](const UserRecord& user) { return user.sid == sid; });
    if (it == m_users.end()) {
        FACELOGIN_WARN(L"LoadCredentialForSid: authorized SID no longer exists");
        return std::nullopt;
    }

    MatchResult result;
    result.distance = distance;
    result.upn = it->upn;
    result.sid = it->sid;
    result.username = it->username;

    auto plain = DpapiUtil::Unprotect(it->encryptedPassword);
    if (!plain.empty()) {
        if (plain.size() % sizeof(wchar_t) == 0) {
            result.password.assign(reinterpret_cast<const wchar_t*>(plain.data()),
                                   plain.size() / sizeof(wchar_t));
        }
        SecureZeroMemory(plain.data(), plain.size());
    }

    if (!result.password.empty()) return result;

    // Password-bearing record whose decrypt failed (e.g. DPAPI key lost) —
    // preserve the previous fail-closed behavior.
    FACELOGIN_WARN(L"LoadCredentialForSid: password decrypt failed for %s",
                   it->username.c_str());
    return std::nullopt;
}

} // namespace facelogin
