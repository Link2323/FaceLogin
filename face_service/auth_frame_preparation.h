#pragma once

#include "../vision/face_gain_tune.h"
#include "auth_frame.h"

namespace facelogin {

// Per-round preparation output. The recovery pointer in tuneConfig points into
// this object, so it must stay at a stable address until authentication ends.
struct PreparedAuthFrames {
    PreparedAuthFrames() = default;
    PreparedAuthFrames(const PreparedAuthFrames&) = delete;
    PreparedAuthFrames& operator=(const PreparedAuthFrames&) = delete;

    AuthFrame seed;
    FaceTuneRecovery tuneRecovery;
    GainTuneConfig tuneConfig;
    bool usingAutoExposure = false;
    bool darkRecoveryAttempted = false;
    std::chrono::steady_clock::time_point gateAt{};
};

} // namespace facelogin
