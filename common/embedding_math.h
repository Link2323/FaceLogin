#pragma once

// Single implementation of the embedding-space math shared by matching
// (credential_store), template learning (template_learner), enrollment
// consistency checks and the ONNX postprocessor. Distances and norms gate
// authentication and learning decisions, so every call site funnels through
// these to keep the metric identical everywhere (double accumulation,
// callers store/compare as float).

#include <cmath>
#include <cstddef>
#include <vector>

namespace facelogin {

// Euclidean distance between two float sequences of length n. Callers
// guarantee n matches both sequences (dimensionality is checked before the
// call — embeddings from different metric spaces are never comparable).
inline double EuclideanDistance(const float* a, const float* b, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

// L2 norm with the same double accumulation (unit-scale embeddings
// depending on the recognizer; see recognizer_profile.h).
inline double L2Norm(const float* v, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    }
    return std::sqrt(sum);
}

inline double L2Norm(const std::vector<float>& v) {
    return L2Norm(v.data(), v.size());
}

} // namespace facelogin
