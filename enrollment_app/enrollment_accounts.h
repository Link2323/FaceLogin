#pragma once

#include "enrollment_templates.h"
#include "service_client.h"
#include "session_identity.h"
#include "../storage/credential_store.h"

namespace facelogin {

struct EnrollmentSaveOutcome {
    bool persisted = false;
    std::string hint;
};

// UI-thread account operations. Owns the enrollment-side database and keeps
// persistence followed by service acknowledgement together. Session lookup and
// password validation remain in session_identity; no plaintext is retained.
class EnrollmentAccounts {
public:
    EnrollmentAccounts(const std::wstring& dataDir,
                       session_identity::SessionIdentity identity,
                       const EnrollmentServiceClient& serviceClient);

    bool SaveTemplates(const std::vector<EnrollmentTemplate>& templates,
                       const std::wstring& password, const std::wstring& label,
                       bool append, EnrollmentSaveOutcome& outcome);
    int GetFaceCount();
    std::string GetFacesJson();
    bool DeleteFace(int faceId);
    bool ClearAllFaces();
    bool RenameFace(int faceId, const std::wstring& label);
    int GetAccountTypeChanged();
    std::string CheckAccountTypeChanged();
    bool RefreshAccountIdentity(const std::wstring& password);
    bool ClearStaleAccountUpn();
    bool AutoRepairEmptyUpnOnStartup();

private:
    struct AccountChange {
        int state = 0;
        size_t faces = 0;
        std::wstring currentUpn;
    };
    AccountChange ReadAccountChange();
    bool SaveAndReload(bool* persisted = nullptr);
    std::string BuildAngleHint() const;

    CredentialStore m_store;
    const session_identity::SessionIdentity m_identity;
    const EnrollmentServiceClient& m_serviceClient;
};

} // namespace facelogin
