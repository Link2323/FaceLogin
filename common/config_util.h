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
    // Template learning (docs/progressive-learning-v2.md). Two independent
    // channels: ema_learning = success-path EMA fine-tune of the matched
    // template after each successful auth; failure_learning = the deferred
    // password-labeled channel (§5 — switch exists, mechanism lands later).
    // Both off = templates are strictly read-only. Renamed from
    // progressive_learning on 2026-09-04 (red line 7: independent switches;
    // the legacy key is still honored on read when the new one is absent).
    bool           ema_learning           = true;
    bool           failure_learning       = true;
    // Per-update blend factor. Clamped to [0.05, 0.15]; effective alpha
    // additionally decays as alpha/(1+n) with n = updates already accepted
    // today (drift guard against same-pose bursts).
    float          learning_alpha         = 0.10f;
    // Update distance gate: only frames matching THIS close may move a
    // template. The stored value is the CAP — the effective gate is
    // min(user's current-era same-person distance p20, cap), tracked by the
    // service per account (docs/progressive-learning-v2.md red line 1).
    // Cap rationale (2026-09-04 final form): 0.65 keeps a margin below the
    // 0.681 minimum measured cross-angle distance while covering the YY
    // environment-drift band; the pose cone + landing-clarity gates (not a
    // wider distance margin) are what block wrong-slot updates. Clamped to
    // [0.35, 0.65].
    float          learning_distance_gate = 0.65f;
    // Absolute garbage line for the pre-normalization embedding norm
    // (w600k_r50 scale ≈ 20-25): blocks only blur/half-face frames. The
    // 2026-09-03 revision rejected the pooled enrollment p25 (20.9) as a
    // hard floor — it would cut roughly half of a dark-scene user's genuine
    // frames (YY-LAPTOP probe p50 20.57). Per-user enrollment p5 is the
    // target (V5); until then the pooled probe p5 ≈ 19.1 is the fallback.
    // Clamped to [15.0, 25.0]. Recalibrate when the recognizer model changes.
    float          learning_norm_floor    = 19.1f;
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
