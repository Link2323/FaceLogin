#pragma once

namespace facelogin {

// Calibration and validation use independent five-frame attempts. Production
// mirrors that policy exactly: five valid frames, and every frame must pass.
inline int AntiSpoofCheckCount(float) {
    return 5;
}

inline int AntiSpoofPassRequired(int checkCount) {
    return checkCount;
}

} // namespace facelogin
