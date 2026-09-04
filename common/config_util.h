#pragma once

#include <string>

namespace facelogin {

struct AppConfig {
    float          match_threshold        = 0.80f;       // 512-D calibrated; EmbeddingThresholdForDim clamps to [0.70, 1.00]
    float          anti_spoof_threshold   = 0.28f;       // 50/50 MiniFAS fusion threshold; UI default on a 0.01 grid (calibration point 0.281)
    // Low-light enhancement. true (default since 2026-08-31) = normalize
    // brightness of dark face chips before recognition. Measured on the
    // production chain: underexposed chips (luma 14-22) cost +0.25-0.30
    // match distance with enhancement off, ~+0.05 with it on — a
    // night-enrolled / day-unlocked (or backlit) setup otherwise lands at
    // the 0.80 threshold's edge. Bright chips are untouched (no-op above
    // luma 40). PAD intentionally keeps the raw preprocessing used during
    // calibration.
    bool           low_light_enhance      = true;
    std::string    camera_device          = "";          // device symbolic link; empty = first camera
    // Camera rotation in degrees clockwise. Valid: 0, 90, 180, 270.
    // Use when the camera is physically mounted in a non-standard
    // orientation (e.g., vertical PC mount / sideways webcam).
    int            camera_rotation        = 0;
    // Progressive template learning (docs/progressive-learning-v2.md §3).
    // EMA fine-tune of the matched face template after each successful auth.
    // master switch off = templates are strictly read-only.
    bool           progressive_learning   = true;
    // Per-update blend factor. Clamped to [0.05, 0.15]; effective alpha
    // additionally decays as alpha/(1+n) with n = updates already accepted
    // today (drift guard against same-pose bursts).
    float          learning_alpha         = 0.10f;
    // Update distance gate: only frames matching THIS close may move a
    // template. Must stay well below match_threshold (impostor frames that
    // barely pass auth can never reach it) and below the minimum measured
    // cross-angle distance (0.681) so a passing frame is same-pose by
    // construction. Clamped to [0.35, 0.55]. Calibrated 2026-09-04.
    float          learning_distance_gate = 0.55f;
    // Pre-normalization embedding norm floor (w600k_r50 scale ≈ 20-25;
    // MagFace-style quality signal). Lower-norm frames may authenticate but
    // never update a template. Recalibrate when the recognizer model changes.
    // Clamped to [15.0, 25.0]. Calibrated 2026-09-04 (enrollment p25 = 20.9).
    float          learning_norm_floor    = 20.9f;
    // Minimum wall-clock spacing between accepted updates. Clamped [10, 3600].
    int            learning_min_interval_sec = 60;
};

AppConfig LoadConfig(const std::wstring& dataDir);
bool      SaveConfig(const std::wstring& dataDir, const AppConfig& cfg);
AppConfig DefaultConfig();

// Serialization
std::string ConfigToJson(const AppConfig& cfg);
AppConfig   ConfigFromJson(const std::string& json);

} // namespace facelogin
