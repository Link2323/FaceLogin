#include "tune_state.h"

#include <cstdio>

namespace facelogin {

// ASCII "exposure=<v>" / "gain=<v>" with optional "exposure_manual=0|1".
// Old two-line states retain their value-only replay behavior. Missing or
// unknown mode data never turns an AE observation into a manual-mode claim.
// Persistence is best-effort; measured tuning stays the authority.
bool LoadTuneKnobState(const std::wstring& path, TuneKnobState& out) {
    out = TuneKnobState{};
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rt") != 0 || !f) return false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long v = 0;
        if (sscanf_s(line, " exposure = %ld", &v) == 1) {
            out.exposure = v;
            out.hasExposure = true;
        } else if (sscanf_s(line, " gain = %ld", &v) == 1) {
            out.gain = v;
            out.hasGain = true;
        } else if (sscanf_s(line, " exposure_manual = %ld", &v) == 1 &&
                   (v == 0 || v == 1)) {
            out.hasExposureMode = true;
            out.exposureManual = v == 1;
        }
    }
    fclose(f);
    return out.hasExposure || out.hasGain;
}

bool SaveTuneKnobState(const std::wstring& path, const TuneKnobState& state) {
    if (!state.hasExposure && !state.hasGain) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wt") != 0 || !f) return false;
    bool written = true;
    if (state.hasExposure) written = fprintf(f, "exposure=%ld\n", state.exposure) >= 0 && written;
    if (state.hasGain) written = fprintf(f, "gain=%ld\n", state.gain) >= 0 && written;
    if (state.hasExposureMode)
        written = fprintf(f, "exposure_manual=%d\n", state.exposureManual ? 1 : 0) >= 0 && written;
    return fclose(f) == 0 && written;
}

} // namespace facelogin
