#pragma once

#include "../vision/onnx_models.h"
#include <chrono>
#include <optional>

namespace facelogin {

// A captured, rotated frame and the detection from that exact frame.
struct AuthFrame {
    bool valid = false;
    FrameImage frame;                       // already rotated
    unsigned long long seq = 0;
    std::optional<OnnxDetector::Detection> det;
    std::chrono::steady_clock::time_point grabWall{};
};

} // namespace facelogin
