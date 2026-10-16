#pragma once

#include <string>

namespace facelogin {

// Synchronous administrative requests. Each method owns its connection,
// bounded wait and CONTROL_ACK; callers never manage a pipe handle.
class EnrollmentServiceClient {
public:
    EnrollmentServiceClient();
    explicit EnrollmentServiceClient(std::wstring pipeName);

    // Database reload retries busy/broken connections; configuration reload
    // allows worker startup. True requires the corresponding service OK.
    bool ReloadDatabase() const;
    bool ReloadConfig() const;
    // Returns the service JSON snapshot or the existing JSON error response.
    std::string QueryLearningStatus() const;

private:
    std::wstring m_pipeName;
};

} // namespace facelogin
