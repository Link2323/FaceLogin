#pragma once

#include <string>

namespace facelogin {

struct AppConfig {
    float          match_threshold        = 0.80f;       // 512-D calibrated; EmbeddingThresholdForDim clamps to [0.70, 1.00]
    float          anti_spoof_threshold   = 0.281f;      // calibrated 50/50 MiniFAS fusion threshold
    // Low-light enhancement. false (default) = no preprocessing. true = apply
    // brightness normalization to dark face chips before recognition. PAD
    // intentionally keeps the raw preprocessing used during calibration.
    bool           low_light_enhance      = false;
    std::string    camera_device          = "";          // device symbolic link; empty = first camera
    // Camera rotation in degrees clockwise. Valid: 0, 90, 180, 270.
    // Use when the camera is physically mounted in a non-standard
    // orientation (e.g., vertical PC mount / sideways webcam).
    int            camera_rotation        = 0;
};

AppConfig LoadConfig(const std::wstring& dataDir);
bool      SaveConfig(const std::wstring& dataDir, const AppConfig& cfg);
AppConfig DefaultConfig();

// Serialization
std::string ConfigToJson(const AppConfig& cfg);
AppConfig   ConfigFromJson(const std::string& json);

} // namespace facelogin
