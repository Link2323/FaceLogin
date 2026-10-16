#pragma once

#include <string>

namespace facelogin {

struct TuneKnobState {
    bool hasExposure = false;
    long exposure = 0;
    bool hasGain = false;
    long gain = 0;
    bool hasExposureMode = false;
    bool exposureManual = false;
};

// Best-effort persistence; missing state never prevents measured tuning.
bool LoadTuneKnobState(const std::wstring& path, TuneKnobState& out);
bool SaveTuneKnobState(const std::wstring& path, const TuneKnobState& state);

} // namespace facelogin
