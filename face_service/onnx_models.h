#pragma once

#include <onnxruntime_cxx_api.h>
#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include "liveness_types.h"

namespace facelogin {

// In-place low-light enhancement for a face chip. Detects darkness (mean
// luma below kLowLightMeanThreshold) and stretches brightness so the mean
// lands at a reference level, clamped to [0,255]. No-op for chips at normal
// brightness. Called on the RESIZED chip before the model's own normalization
// loop, so InsightFace sees brightness-normalized input in dark scenes.
//
// Safe by construction: only affects genuinely dark chips; a normal-brightness
// chip is returned unchanged, so the match threshold and photo-rejection
// boundary are untouched.
void ApplyLowLightEnhance(dlib::matrix<dlib::rgb_pixel>& chip);

// ONNX-based face recognition using InsightFace (w600k_mbf / w600k_r50).
// Embedding dimension: 512 (auto-detected from the model output).
class OnnxRecognizer {
public:
    OnnxRecognizer() = default;
    ~OnnxRecognizer();

    bool Initialize(const std::wstring& modelPath);

    // Compute 512-D embedding from a face chip (already aligned, 112x112 RGB).
    // Returns empty vector on failure.
    std::vector<float> ComputeEmbedding(const dlib::matrix<dlib::rgb_pixel>& faceChip);

    // Convenience: compute embedding from a full frame + the 5 SCRFD
    // keypoints (source-pixel coordinates). Aligns to 112×112 internally
    // via a similarity transform (see face_align.h).
    std::vector<float> ComputeEmbedding(const dlib::matrix<dlib::rgb_pixel>& image,
                                        const float kps[10]);

    bool IsInitialized() const { return m_initialized; }

    // Enable/disable low-light brightness normalization for dark face chips.
    void SetLowLightEnhance(bool enable) { m_lowLightEnhance = enable; }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    bool m_initialized = false;
    bool m_lowLightEnhance = false;

    // Input/output names (cached after session creation)
    std::string m_inputName;
    std::string m_outputName;
};

// ONNX-based face detection using InsightFace SCRFD.
// Much faster than dlib HOG and more robust against non-live faces.
class OnnxDetector {
public:
    OnnxDetector() = default;
    ~OnnxDetector();

    bool Initialize(const std::wstring& modelPath);

    struct Detection {
        float x1, y1, x2, y2;  // bounding box in pixel coordinates
        float score;             // confidence
        float kps[10];           // 5 keypoints (x,y pairs): left-eye, right-eye, nose, left-mouth, right-mouth
    };

    // Detect faces. Returns detections sorted by confidence (highest first).
    std::vector<Detection> Detect(const dlib::matrix<dlib::rgb_pixel>& image);

    // Detect the largest face (by area). Returns nullopt if none found.
    std::optional<Detection> DetectLargestFace(const dlib::matrix<dlib::rgb_pixel>& image);

    bool IsInitialized() const { return m_initialized; }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    bool m_initialized = false;

    std::string m_inputName;
    std::vector<std::string> m_outputNames;

    // Preprocess: direct resize to 640×640 (no letterbox), BGR planar,
    // normalized to [-1, 1] with (pixel-127.5)/128. Matches the model's
    // native input; insightface SCRFD is exported this way.
    // Because the resize DISTORTS non-square frames (e.g. 1280×720 → 640×640),
    // the x and y scales are DIFFERENT. scaleX/scaleY map 640-space back to
    // source pixels: srcX = detX * scaleX, srcY = detY * scaleY.
    std::vector<float> Preprocess(const dlib::matrix<dlib::rgb_pixel>& image,
                                   float& outScaleX, float& outScaleY);
};

// One MiniFASNet ONNX model. The reference implementation expands the SCRFD
// face box, resizes it to the model input, feeds raw BGR NCHW values, and uses
// softmax class 1 as the real-face probability.
class MiniFasEvaluator {
public:
    MiniFasEvaluator() = default;
    ~MiniFasEvaluator();

    MiniFasEvaluator(const MiniFasEvaluator&) = delete;
    MiniFasEvaluator& operator=(const MiniFasEvaluator&) = delete;

    bool Initialize(const std::wstring& modelPath, float cropScale);
    float Predict(const dlib::matrix<dlib::rgb_pixel>& image,
                  const dlib::rectangle& faceRect);
    bool IsInitialized() const { return m_initialized; }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    std::string m_inputName;
    std::string m_outputName;
    int m_inputHeight = 0;
    int m_inputWidth = 0;
    float m_cropScale = 0.0f;
    bool m_initialized = false;
};

// Production PAD: MiniFASNetV2 (2.7x crop) and MiniFASNetV1SE (4.0x crop)
// evaluated concurrently (each owns its own session/env), then fused with an
// equal-weight arithmetic mean.  If either model fails, the fused prediction
// fails closed.
class OnnxAntiSpoof {
public:
    OnnxAntiSpoof() = default;
    ~OnnxAntiSpoof();

    bool Initialize(const std::wstring& miniFasV2Path,
                    const std::wstring& miniFasV1SePath);

    // Returns the 50/50 fused real-face probability, or -1 on any model error.
    float Predict(const dlib::matrix<dlib::rgb_pixel>& image,
                  const dlib::rectangle& rect);

    bool IsInitialized() const { return m_initialized; }

private:
    std::unique_ptr<MiniFasEvaluator> m_miniFasV2;
    std::unique_ptr<MiniFasEvaluator> m_miniFasV1Se;
    bool m_initialized = false;
};

} // namespace facelogin
