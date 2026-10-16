#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace facelogin {

// Pinned OpenCV SFace 2021dec FP32, with initializer graph inputs removed.
// Weight values/nodes and the 128-D metric space are preserved.
inline constexpr size_t kRecognizerDimension = 128;
inline constexpr uint32_t kCredentialDatabaseVersion = 6;
inline constexpr uint32_t kRecognizerModelTag = 0x31434653; // "SFC1"
// Conservative trial ceiling, below the offline 1.0693 exploratory cutoff.
// Not a calibrated production FAR guarantee; keep ratio=0.75 and mandatory PAD.
inline constexpr float kDefaultMatchThreshold = 1.00f;
inline constexpr float kMinMatchThreshold = 0.70f;
inline constexpr float kMaxMatchThreshold = 1.00f;
inline constexpr bool kRecognizerLearningValidated = false;

inline float NormalizeMatchThreshold(float value) {
    if (!std::isfinite(value) || value < kMinMatchThreshold)
        return kDefaultMatchThreshold;
    return value > kMaxMatchThreshold ? kMaxMatchThreshold : value;
}

} // namespace facelogin
