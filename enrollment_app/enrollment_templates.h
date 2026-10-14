#pragma once

#include "enrollment_samples.h"

#include <string>
#include <vector>

namespace facelogin {

struct EnrollmentTemplate {
    std::vector<float> embedding;
    std::wstring label;
    int targetYaw = 0;
};

// Validates consistency and produces one normalized template per angle.
// No camera, inference, credential or database access. On failure out is empty;
// callers prepare every angle successfully before mutating account records.
bool BuildEnrollmentTemplates(const EnrollmentSamples& samples,
                              std::vector<EnrollmentTemplate>& out);

} // namespace facelogin
