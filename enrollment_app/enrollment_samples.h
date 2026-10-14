#pragma once

#include <cstddef>
#include <vector>

namespace facelogin {

struct EnrollmentSamples {
    struct AngleGroup {
        size_t begin;
        size_t count;
        int angle;
        const wchar_t* label;
        int targetYaw;
    };
    std::vector<std::vector<float>> embeddings;
    std::vector<AngleGroup> groups;
};

} // namespace facelogin
