#include "onnx_models.h"
#include "face_align.h"
#include "../common/logger.h"
#include <fstream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <future>
#include <cstdlib>

namespace facelogin {

// ONNX intra-op thread count. Defaults to half the machine's logical cores
// (capped at 8 — measured on a 16c/32t Ryzen 9 7945HX, raising from 4→8 cut
// w600k_r50 per-call latency with no system-lag regression; see
// docs/performance-baseline.md perf2). Lower-core machines naturally use fewer
// via the cores/2 floor. Overridable via FACELOGIN_ONNX_THREADS env var.
// Inter-op parallelism is pinned to 1 (our callers are single-graph, so a
// second inter-op pool only adds contention without throughput benefit).
static int OnnxThreadCount() {
    if (const char* env = std::getenv("FACELOGIN_ONNX_THREADS")) {
        int n = std::atoi(env);
        if (n > 0) return n;
    }
    int n = static_cast<int>(std::thread::hardware_concurrency());
    if (n <= 0) return 2;
    n = std::max(2, n / 2);   // half of logical cores
    if (n > 8) return 8;
    return n;
}

// ONNX Runtime documents Ort::Env as the process-level runtime environment and
// provides global thread pools for sessions that are repeatedly constructed.
// A shared Env alone is insufficient: the default SessionOptions still creates
// an 8-thread pool for every detector/recognizer session, and the Windows thread
// handles remained after teardown (~17 handles per lock cycle in production).
// Keep the pools process-long and unload only sessions/model weights.
static std::unique_ptr<Ort::Env> CreateProcessOrtEnv(int intraOpThreads,
                                                      const char* logId) {
    Ort::ThreadingOptions threadingOptions;
    threadingOptions.SetGlobalIntraOpNumThreads(intraOpThreads);
    threadingOptions.SetGlobalInterOpNumThreads(1);
    return std::make_unique<Ort::Env>(threadingOptions,
                                      ORT_LOGGING_LEVEL_WARNING, logId);
}

static Ort::Env& ProcessOrtEnv() {
    static auto env = CreateProcessOrtEnv(OnnxThreadCount(), "FaceLogin");
    return *env;
}

// MiniFAS was calibrated with one intra-op thread per model. It runs two
// sessions concurrently on their caller threads, so use a separate one-thread
// global pool rather than letting both small graphs contend for the 8-thread
// detector/recognizer pool.
static Ort::Env& ProcessMiniFasOrtEnv() {
    static auto env = CreateProcessOrtEnv(1, "FaceLoginMiniFAS");
    return *env;
}

// ============================================================================
// Low-light enhancement (recognizer)
// ============================================================================

// A chip is "dark" when its mean luma is below ~40/255 (0.157). Normal indoor
// faces are 100-180; genuinely dark scenes fall well below 40.
static constexpr float kLowLightMeanThreshold = 40.0f;
// Reference mean luma we stretch dark chips toward. ~110/255 ≈ mid-brightness,
// close to what InsightFace was trained on.
static constexpr float kLowLightTargetMean   = 110.0f;

void ApplyLowLightEnhance(FrameImage& chip) {
    const long n = static_cast<long>(chip.size());
    if (n == 0) return;

    // Mean luma over the chip.
    double sum = 0.0;
    for (long i = 0; i < n; i++) {
        const auto& p = chip(i);
        sum += 0.299 * p.red + 0.587 * p.green + 0.114 * p.blue;
    }
    float mean = static_cast<float>(sum / n);
    if (mean >= kLowLightMeanThreshold) return;  // not dark — no-op

    // Stretch brightness: gain brings the mean up to the target, clamped so a
    // bright pixel can't overflow past 255.
    float gain = kLowLightTargetMean / mean;
    for (long i = 0; i < n; i++) {
        auto& p = chip(i);
        int r = static_cast<int>(p.red   * gain + 0.5f);
        int g = static_cast<int>(p.green * gain + 0.5f);
        int b = static_cast<int>(p.blue  * gain + 0.5f);
        p.red   = static_cast<unsigned char>(r > 255 ? 255 : r);
        p.green = static_cast<unsigned char>(g > 255 ? 255 : g);
        p.blue  = static_cast<unsigned char>(b > 255 ? 255 : b);
    }
}

// ============================================================================
// OnnxRecognizer
// ============================================================================

OnnxRecognizer::~OnnxRecognizer() = default;

bool OnnxRecognizer::Initialize(const std::wstring& modelPath) {
    try {
        Ort::SessionOptions opts;
        opts.DisablePerSessionThreads();
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::wstring wpath(modelPath.begin(), modelPath.end());
        m_session = std::make_unique<Ort::Session>(ProcessOrtEnv(), wpath.c_str(), opts);

        m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        // Cache input/output names
        Ort::AllocatorWithDefaultOptions alloc;
        m_inputName = m_session->GetInputNameAllocated(0, alloc).get();
        m_outputName = m_session->GetOutputNameAllocated(0, alloc).get();

        FACELOGIN_INFO(L"OnnxRecognizer initialized: %s", modelPath.c_str());
        FACELOGIN_INFO(L"  Input: %hs, Output: %hs", m_inputName.c_str(), m_outputName.c_str());
        // Diagnostic for slow-machine reports (e.g. i5-13500H embedding ~1.4s):
        // the thread count is derived from hardware_concurrency, which can be
        // wrong in Session 0 or masked by power policies, and threads can land
        // on E-cores. Knowing the actual value distinguishes "few threads"
        // from "threads present but slow" in one log line.
        FACELOGIN_INFO(L"  ONNX global intra-op threads: %d (hardware_concurrency=%zu)",
                       OnnxThreadCount(),
                       static_cast<size_t>(std::thread::hardware_concurrency()));

        m_initialized = true;
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"OnnxRecognizer init failed: %hs", e.what());
        return false;
    }
}

std::vector<float> OnnxRecognizer::ComputeEmbedding(
    const FrameImage& faceChip) {
    if (!m_initialized) return {};

    try {
        int h = static_cast<int>(faceChip.nr());
        int w = static_cast<int>(faceChip.nc());

        // InsightFace buffalo_s expects 112x112 RGB, normalized to [-1, 1]
        // First resize to 112x112
        FrameImage resized(112, 112);
        ResizeBilinear(faceChip, resized);

        // Optional low-light enhancement (config-gated): normalize brightness
        // of dark chips so the embedding isn't distorted by a dark scene.
        if (m_lowLightEnhance) ApplyLowLightEnhance(resized);

        // Convert to NCHW float tensor: [1, 3, 112, 112] normalized to [-1, 1]
        std::vector<float> input(1 * 3 * 112 * 112);
        const float scale = 1.0f / 127.5f;
        for (int y = 0; y < 112; y++) {
            for (int x = 0; x < 112; x++) {
                const auto& p = resized(y, x);
                int base = y * 112 + x;
                input[0 * 112 * 112 + base] = static_cast<float>(p.red)   * scale - 1.0f;
                input[1 * 112 * 112 + base] = static_cast<float>(p.green) * scale - 1.0f;
                input[2 * 112 * 112 + base] = static_cast<float>(p.blue)  * scale - 1.0f;
            }
        }

        std::array<int64_t, 4> shape = {1, 3, 112, 112};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, input.data(), input.size(), shape.data(), shape.size());

        const char* inputNames[] = {m_inputName.c_str()};
        const char* outName = m_outputName.c_str();
        const char* outputNames[] = {outName};
        auto outputs = m_session->Run(Ort::RunOptions{},
                                       inputNames, &inputTensor, 1,
                                       outputNames, 1);

        float* data = outputs[0].GetTensorMutableData<float>();
        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        size_t dim = info.GetElementCount();

        std::vector<float> embedding(data, data + dim);

        // L2 normalize
        float norm = 0.0f;
        for (float v : embedding) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 1e-8f) {
            for (float& v : embedding) v /= norm;
        }

        return embedding;
    } catch (const std::exception& e) {
        FACELOGIN_WARN(L"OnnxRecognizer::ComputeEmbedding error: %hs", e.what());
        return {};
    }
}

std::vector<float> OnnxRecognizer::ComputeEmbedding(
    const FrameImage& image, const float kps[10]) {
    // Align via 5-point similarity transform (InsightFace convention), then
    // ONNX infer. No landmark model involved — SCRFD provides the keypoints.
    FrameImage faceChip;
    if (!AlignFace5(image, kps, 112, faceChip)) return {};
    return ComputeEmbedding(faceChip);
}

// ============================================================================
// OnnxDetector
// ============================================================================

OnnxDetector::~OnnxDetector() = default;

bool OnnxDetector::Initialize(const std::wstring& modelPath) {
    try {
        Ort::SessionOptions opts;
        opts.DisablePerSessionThreads();
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        std::wstring wpath(modelPath.begin(), modelPath.end());
        m_session = std::make_unique<Ort::Session>(ProcessOrtEnv(), wpath.c_str(), opts);

        m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        Ort::AllocatorWithDefaultOptions alloc;
        m_inputName = m_session->GetInputNameAllocated(0, alloc).get();

        size_t numOutputs = m_session->GetOutputCount();
        m_outputNames.resize(numOutputs);
        for (size_t i = 0; i < numOutputs; i++) {
            m_outputNames[i] = m_session->GetOutputNameAllocated(i, alloc).get();
        }

        FACELOGIN_INFO(L"OnnxDetector initialized: %s", modelPath.c_str());
        m_initialized = true;
        return true;
    } catch (const std::exception& e) {
        FACELOGIN_ERROR(L"OnnxDetector init failed: %hs", e.what());
        return false;
    }
}

std::vector<float> OnnxDetector::Preprocess(const FrameImage& image,
                                              float& outScaleX, float& outScaleY) {
    int srcH = static_cast<int>(image.nr());
    int srcW = static_cast<int>(image.nc());

    // SCRFD model input is dynamic; we feed 512×512 (was 640×640).  Validated
    // 2026-08 over 60 photos: 60/60 detected (640 missed 1), box IoU 0.96,
    // detect time -30% (memory-bandwidth-bound on weak CPUs, so INT8 did not
    // help but smaller input does).  insightface's official SCRFD detect()
    // does a DIRECT resize (no letterbox, no aspect-preserving pad) — the
    // square blob is a full distort of the source frame. Using the same
    // preprocessing is essential; a letterbox would shift the feature-map
    // anchors and produce misaligned boxes.
    //
    // CRITICAL: because the resize DISTORTS non-square frames (e.g. a 1280×720
    // camera frame is squeezed into 512×512), the x and y scale factors are
    // DIFFERENT. Using a single uniform scale here misplaces boxes by the
    // aspect-ratio difference — for 1280×720 a uniform factor pushes boxes past
    // the frame edge, so dlib landmark extraction fails and auth times out.
    const int targetSize = 512;
    outScaleX = static_cast<float>(srcW) / static_cast<float>(targetSize);
    outScaleY = static_cast<float>(srcH) / static_cast<float>(targetSize);

    FrameImage resized(targetSize, targetSize);
    ResizeBilinear(image, resized);

    // BGR planar NCHW, normalized to [-1, 1] with (pixel - 127.5) / 128.
    // insightface normalizes with 128, NOT 255.
    std::vector<float> tensor(1 * 3 * targetSize * targetSize, 0.0f);
    for (int y = 0; y < targetSize; y++) {
        for (int x = 0; x < targetSize; x++) {
            const auto& p = resized(y, x);
            int base = y * targetSize + x;
            tensor[0 * targetSize * targetSize + base] = (static_cast<float>(p.blue)  - 127.5f) / 128.0f;
            tensor[1 * targetSize * targetSize + base] = (static_cast<float>(p.green) - 127.5f) / 128.0f;
            tensor[2 * targetSize * targetSize + base] = (static_cast<float>(p.red)   - 127.5f) / 128.0f;
        }
    }

    return tensor;
}

// SCRFD decode helpers (matching insightface's scrfd.py).
//
// SCRFD bbox output is distance-based: 4 channels [left, top, right, bottom]
// in units of stride steps from the anchor center. keypoints output is 10
// channels (5 points × 2), also relative to the anchor center.

// Convert (center, distance) predictions into a bounding box [x1,y1,x2,y2].
static inline void DistanceToBbox(float cx, float cy,
                                  const float* dist,
                                  float& x1, float& y1, float& x2, float& y2) {
    x1 = cx - dist[0];
    y1 = cy - dist[1];
    x2 = cx + dist[2];
    y2 = cy + dist[3];
}

// Convert (center, distance) predictions into a 5-keypoint list (10 floats).
static inline void DistanceToKps(float cx, float cy,
                                 const float* dist, float* kps) {
    for (int k = 0; k < 5; k++) {
        kps[k * 2]     = cx + dist[k * 2];
        kps[k * 2 + 1] = cy + dist[k * 2 + 1];
    }
}

std::vector<OnnxDetector::Detection> OnnxDetector::Detect(
    const FrameImage& image) {
    std::vector<Detection> results;
    if (!m_initialized) return results;

    try {
        float scaleX = 0.0f, scaleY = 0.0f;
        auto input = Preprocess(image, scaleX, scaleY);

        // Fixed 512×512 model input (dynamic-dimension graph, see Preprocess).
        const int inputSize = 512;
        std::array<int64_t, 4> shape = {1, 3, inputSize, inputSize};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, input.data(), input.size(), shape.data(), shape.size());

        const char* inputNames2[] = {m_inputName.c_str()};
        std::vector<const char*> outNames;
        for (auto& name : m_outputNames) outNames.push_back(name.c_str());
        std::vector<Ort::Value> outputTensors = m_session->Run(
            Ort::RunOptions{},
            inputNames2, &inputTensor, 1,
            outNames.data(), outNames.size());

        // SCRFD outputs (9 tensors): for each of 3 strides {8, 16, 32}:
        //   scores  [N, 1]
        //   bboxes  [N, 4]   (distance-to-center: l,t,r,b in stride units)
        //   kps     [N, 10]  (5 points × 2, also distance-to-center)
        // N = (512/stride)² × 2 (num_anchors=2), centers repeated per anchor.
        constexpr int kStrides[3] = {8, 16, 32};
        constexpr float kScoreThreshold = 0.5f;

        struct RawDet {
            float score;
            float box[4];
            float kps[10];
        };
        std::vector<RawDet> raw;

        for (int s = 0; s < 3; s++) {
            int stride = kStrides[s];
            int grid = inputSize / stride;
            size_t numPts = static_cast<size_t>(grid) * grid;
            size_t numAnchors = numPts * 2;   // num_anchors = 2

            float* scoreData = outputTensors[s].GetTensorMutableData<float>();
            float* boxData   = outputTensors[3 + s].GetTensorMutableData<float>();
            float* kpsData   = outputTensors[6 + s].GetTensorMutableData<float>();

            // Precompute anchor centers for this stride, following insightface's
            // official scrfd.py exactly:
            //   anchor_centers = np.stack(np.mgrid[:height, :width][::-1], axis=-1)
            //                   .reshape(-1, 2) * stride
            // np.mgrid[:h,:w][::-1] produces ROW-major (x varies fastest = columns
            // inner loop). Center = (col, row) * stride, NO +0.5 offset.
            // Both anchors of a cell share this center; output is interleaved:
            //   index i → cell i/2, anchor i%2.
            std::vector<float> centerX(numPts), centerY(numPts);
            for (int r = 0; r < grid; r++) {          // row outer
                for (int c = 0; c < grid; c++) {      // col inner (x fastest)
                    centerX[r * grid + c] = c * stride;
                    centerY[r * grid + c] = r * stride;
                }
            }

            for (size_t i = 0; i < numAnchors; i++) {
                float score = scoreData[i];
                if (score < kScoreThreshold) continue;

                size_t cell = i / 2;   // both anchors share this cell's center
                float cx = centerX[cell];
                float cy = centerY[cell];

                RawDet det;
                det.score = score;

                // bbox distance is in stride units → multiply by stride.
                float dist[4] = { boxData[i * 4 + 0] * stride,
                                  boxData[i * 4 + 1] * stride,
                                  boxData[i * 4 + 2] * stride,
                                  boxData[i * 4 + 3] * stride };
                float x1, y1, x2, y2;
                DistanceToBbox(cx, cy, dist, x1, y1, x2, y2);
                det.box[0] = x1; det.box[1] = y1; det.box[2] = x2; det.box[3] = y2;

                // keypoints distance in stride units too.
                float kd[10];
                for (int k = 0; k < 10; k++) kd[k] = kpsData[i * 10 + k] * stride;
                DistanceToKps(cx, cy, kd, det.kps);

                raw.push_back(det);
            }
        }

        // Non-maximum suppression across all strides (score-descending greedy).
        std::sort(raw.begin(), raw.end(),
            [](const RawDet& a, const RawDet& b) { return a.score > b.score; });

        constexpr float kNmsIoU = 0.5f;
        std::vector<bool> suppressed(raw.size(), false);
        for (size_t i = 0; i < raw.size(); i++) {
            if (suppressed[i]) continue;
            const RawDet& a = raw[i];
            float ax1 = a.box[0], ay1 = a.box[1], ax2 = a.box[2], ay2 = a.box[3];
            float aArea = (ax2 - ax1) * (ay2 - ay1) + 1e-5f;

            for (size_t j = i + 1; j < raw.size(); j++) {
                if (suppressed[j]) continue;
                const RawDet& b = raw[j];
                float ix = std::min(ax2, b.box[2]) - std::max(ax1, b.box[0]);
                float iy = std::min(ay2, b.box[3]) - std::max(ay1, b.box[1]);
                if (ix <= 0 || iy <= 0) continue;
                float inter = ix * iy;
                float bArea = (b.box[2] - b.box[0]) * (b.box[3] - b.box[1]) + 1e-5f;
                float iou = inter / (aArea + bArea - inter + 1e-5f);
                if (iou > kNmsIoU) suppressed[j] = true;
            }
        }

        // Map surviving detections back to source-pixel coordinates. The model
        // DISTORTS non-square frames, so x and y scale independently.
        for (size_t i = 0; i < raw.size(); i++) {
            if (suppressed[i]) continue;
            const RawDet& d = raw[i];
            Detection det;
            det.x1 = d.box[0] * scaleX;
            det.y1 = d.box[1] * scaleY;
            det.x2 = d.box[2] * scaleX;
            det.y2 = d.box[3] * scaleY;
            det.score = d.score;
            for (int k = 0; k < 5; k++) {
                det.kps[k * 2]     = d.kps[k * 2]     * scaleX;
                det.kps[k * 2 + 1] = d.kps[k * 2 + 1] * scaleY;
            }
            results.push_back(det);
        }

        // Sort by confidence descending (already score-sorted after NMS, but
        // keep it explicit for the public contract).
        std::sort(results.begin(), results.end(),
            [](const Detection& a, const Detection& b) { return a.score > b.score; });

    } catch (const std::exception& e) {
        FACELOGIN_WARN(L"OnnxDetector::Detect error: %hs", e.what());
    }

    return results;
}

std::optional<OnnxDetector::Detection> OnnxDetector::DetectLargestFace(
    const FrameImage& image) {
    auto detections = Detect(image);
    if (detections.empty()) return std::nullopt;

    auto largest = std::max_element(detections.begin(), detections.end(),
        [](const Detection& a, const Detection& b) {
            float areaA = (a.x2 - a.x1) * (a.y2 - a.y1);
            float areaB = (b.x2 - b.x1) * (b.y2 - b.y1);
            return areaA < areaB;
        });

    return *largest;
}

// ============================================================================
// MiniFASNet model and production 50/50 fusion
// ============================================================================

MiniFasEvaluator::~MiniFasEvaluator() = default;

bool MiniFasEvaluator::Initialize(const std::wstring& modelPath, float cropScale) {
    m_initialized = false;
    m_inputName.clear();
    m_outputName.clear();
    m_inputHeight = 0;
    m_inputWidth = 0;
    m_cropScale = 0.0f;

    if (!std::isfinite(cropScale) || cropScale < 1.0f || cropScale > 8.0f) {
        FACELOGIN_ERROR(L"MiniFASNet invalid crop scale: %.3f", cropScale);
        return false;
    }

    try {
        Ort::SessionOptions options;
        // These small models benchmark fastest and most consistently with the
        // separate one-thread process pool above.
        options.DisablePerSessionThreads();
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_session = std::make_unique<Ort::Session>(ProcessMiniFasOrtEnv(),
                                                   modelPath.c_str(), options);
        m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        if (m_session->GetInputCount() != 1 || m_session->GetOutputCount() != 1) {
            FACELOGIN_ERROR(L"MiniFASNet graph must have exactly one input and one output");
            return false;
        }

        const auto inputInfo = m_session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto inputShape = inputInfo.GetShape();
        if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            inputShape.size() != 4 || inputShape[1] != 3 ||
            inputShape[2] <= 0 || inputShape[3] <= 0 ||
            inputShape[2] > 1024 || inputShape[3] > 1024) {
            FACELOGIN_ERROR(L"MiniFASNet invalid input tensor; expected float NCHW with 3 channels");
            return false;
        }

        const auto outputInfo = m_session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const auto outputShape = outputInfo.GetShape();
        if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            outputShape.empty() || outputShape.back() != 3) {
            FACELOGIN_ERROR(L"MiniFASNet invalid output tensor; expected three class logits");
            return false;
        }

        Ort::AllocatorWithDefaultOptions allocator;
        m_inputName = m_session->GetInputNameAllocated(0, allocator).get();
        m_outputName = m_session->GetOutputNameAllocated(0, allocator).get();
        if (m_inputName.empty() || m_outputName.empty()) return false;

        m_inputHeight = static_cast<int>(inputShape[2]);
        m_inputWidth = static_cast<int>(inputShape[3]);
        m_cropScale = cropScale;
        m_initialized = true;
        FACELOGIN_INFO(L"MiniFASNet initialized: %s (%dx%d, crop %.1f)",
                       modelPath.c_str(), m_inputWidth, m_inputHeight, m_cropScale);
        return true;
    } catch (const std::exception& error) {
        FACELOGIN_ERROR(L"MiniFASNet initialization failed: %hs", error.what());
        return false;
    }
}

float MiniFasEvaluator::Predict(const FrameImage& image,
                                const FaceRect& faceRect) {
    if (!m_initialized || !m_session || !m_memoryInfo || image.size() == 0 ||
        faceRect.is_empty() || m_inputHeight <= 0 || m_inputWidth <= 0) {
        return -1.0f;
    }

    try {
        const double sourceWidth = static_cast<double>(image.nc());
        const double sourceHeight = static_cast<double>(image.nr());
        const double boxWidth = static_cast<double>(faceRect.width());
        const double boxHeight = static_cast<double>(faceRect.height());
        if (boxWidth <= 1.0 || boxHeight <= 1.0) return -1.0f;

        // Faithful port of Silent-Face-Anti-Spoofing's CropImage._get_new_box.
        const double scale = std::min({
            (sourceHeight - 1.0) / boxHeight,
            (sourceWidth - 1.0) / boxWidth,
            static_cast<double>(m_cropScale)
        });
        const double newWidth = boxWidth * scale;
        const double newHeight = boxHeight * scale;
        const double centerX = static_cast<double>(faceRect.left()) + boxWidth / 2.0;
        const double centerY = static_cast<double>(faceRect.top()) + boxHeight / 2.0;

        double left = centerX - newWidth / 2.0;
        double top = centerY - newHeight / 2.0;
        double right = centerX + newWidth / 2.0;
        double bottom = centerY + newHeight / 2.0;
        if (left < 0.0) { right -= left; left = 0.0; }
        if (top < 0.0) { bottom -= top; top = 0.0; }
        if (right > sourceWidth - 1.0) {
            left -= right - sourceWidth + 1.0;
            right = sourceWidth - 1.0;
        }
        if (bottom > sourceHeight - 1.0) {
            top -= bottom - sourceHeight + 1.0;
            bottom = sourceHeight - 1.0;
        }

        const FaceRect cropRect(
            std::max(0L, static_cast<long>(left)),
            std::max(0L, static_cast<long>(top)),
            std::min(image.nc() - 1, static_cast<long>(right)),
            std::min(image.nr() - 1, static_cast<long>(bottom)));
        if (cropRect.is_empty()) return -1.0f;

        FrameImage crop;
        ExtractChip(image, cropRect, m_inputHeight, m_inputWidth, crop);

        const size_t plane = static_cast<size_t>(m_inputHeight) *
                             static_cast<size_t>(m_inputWidth);
        std::vector<float> input(plane * 3U);
        for (int y = 0; y < m_inputHeight; ++y) {
            for (int x = 0; x < m_inputWidth; ++x) {
                const auto& pixel = crop(y, x);
                const size_t offset = static_cast<size_t>(y) *
                                      static_cast<size_t>(m_inputWidth) +
                                      static_cast<size_t>(x);
                input[offset] = static_cast<float>(pixel.blue);
                input[plane + offset] = static_cast<float>(pixel.green);
                input[plane * 2U + offset] = static_cast<float>(pixel.red);
            }
        }

        const std::array<int64_t, 4> shape = {
            1, 3, static_cast<int64_t>(m_inputHeight), static_cast<int64_t>(m_inputWidth)
        };
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            *m_memoryInfo, input.data(), input.size(), shape.data(), shape.size());
        const char* inputNames[] = {m_inputName.c_str()};
        const char* outputNames[] = {m_outputName.c_str()};
        auto outputs = m_session->Run(Ort::RunOptions{}, inputNames, &tensor, 1,
                                      outputNames, 1);
        if (outputs.size() != 1 || !outputs[0].IsTensor()) return -1.0f;

        const auto outputInfo = outputs[0].GetTensorTypeAndShapeInfo();
        if (outputInfo.GetElementCount() != 3) return -1.0f;
        const float* logits = outputs[0].GetTensorData<float>();
        if (!logits || !std::isfinite(logits[0]) || !std::isfinite(logits[1]) ||
            !std::isfinite(logits[2])) {
            return -1.0f;
        }

        const float maximum = std::max({logits[0], logits[1], logits[2]});
        const double exp0 = std::exp(static_cast<double>(logits[0] - maximum));
        const double exp1 = std::exp(static_cast<double>(logits[1] - maximum));
        const double exp2 = std::exp(static_cast<double>(logits[2] - maximum));
        const double total = exp0 + exp1 + exp2;
        if (!std::isfinite(total) || total <= std::numeric_limits<double>::min()) {
            return -1.0f;
        }
        const float realProbability = static_cast<float>(exp1 / total);
        return std::isfinite(realProbability) ? realProbability : -1.0f;
    } catch (const std::exception& error) {
        FACELOGIN_WARN(L"MiniFASNet inference failed: %hs", error.what());
        return -1.0f;
    }
}

OnnxAntiSpoof::~OnnxAntiSpoof() = default;

bool OnnxAntiSpoof::Initialize(const std::wstring& miniFasV2Path,
                               const std::wstring& miniFasV1SePath) {
    m_initialized = false;
    m_miniFasV2 = std::make_unique<MiniFasEvaluator>();
    m_miniFasV1Se = std::make_unique<MiniFasEvaluator>();

    const bool v2Ready = m_miniFasV2->Initialize(miniFasV2Path, 2.7f);
    const bool v1SeReady = m_miniFasV1Se->Initialize(miniFasV1SePath, 4.0f);
    if (!v2Ready || !v1SeReady) {
        FACELOGIN_ERROR(L"Dual MiniFAS PAD initialization failed (V2=%s, V1SE=%s)",
                        v2Ready ? L"ready" : L"failed",
                        v1SeReady ? L"ready" : L"failed");
        m_miniFasV2.reset();
        m_miniFasV1Se.reset();
        return false;
    }

    m_initialized = true;
    FACELOGIN_INFO(L"Dual MiniFAS PAD initialized (V2 + V1SE, 50/50 fusion)");
    return true;
}

float OnnxAntiSpoof::Predict(const FrameImage& image,
                             const FaceRect& rect) {
    if (!m_initialized || !m_miniFasV2 || !m_miniFasV1Se) return -1.0f;

    // V2 and V1SE are independent (each owns its own env/session, both pinned to
    // a single intra-op thread, share no mutable state), so they can run
    // concurrently.  This roughly halves the PAD inference portion of each
    // anti-spoof frame.  Both operate on the same input image/rect, which is
    // only read (FrameImage is not mutated by Predict), so the aliasing is
    // safe.  std::async with std::launch::async guarantees a real thread.
    auto v2Future = std::async(std::launch::async,
        [&] { return m_miniFasV2->Predict(image, rect); });
    const float v1SeScore = m_miniFasV1Se->Predict(image, rect);
    const float v2Score = v2Future.get();
    if (!std::isfinite(v2Score) || !std::isfinite(v1SeScore) ||
        v2Score < 0.0f || v2Score > 1.0f ||
        v1SeScore < 0.0f || v1SeScore > 1.0f) {
        FACELOGIN_WARN(L"Dual MiniFAS PAD failed closed (V2=%.4f, V1SE=%.4f)",
                       v2Score, v1SeScore);
        return -1.0f;
    }

    const float fusedScore = (v2Score + v1SeScore) * 0.5f;
    // Per-frame PAD breakdown — DEBUG (only in standalone); the pass/fail tally
    // in ProcessAuthRequest is enough at INFO level.
    FACELOGIN_DEBUG(L"Dual MiniFAS PAD: V2=%.4f V1SE=%.4f fused=%.4f",
                    v2Score, v1SeScore, fusedScore);
    return fusedScore;
}

void WarmupInference(OnnxDetector& detector,
                     OnnxRecognizer& recognizer,
                     OnnxAntiSpoof& antiSpoof) {
    const auto start = std::chrono::steady_clock::now();

    // Mid-gray dummies keep low-light enhance a no-op (mean luma 128, well
    // above the 40 darkness threshold) and reproduce the production tensor
    // shapes: the 640×480 frame feeds the same 512×512 detector preprocess and
    // MiniFAS crop path, the 112×112 chip the recognizer input.
    FrameImage frame(480, 640);
    std::fill(frame.raw(), frame.raw() + frame.size() * 3, 128);
    FrameImage chip(112, 112);
    std::fill(chip.raw(), chip.raw() + chip.size() * 3, 128);

    detector.DetectLargestFace(frame);   // uniform gray — nullopt is expected
    if (antiSpoof.Predict(frame, FaceRect(220, 140, 419, 339)) < 0.0f) {
        FACELOGIN_WARN(L"Warmup PAD inference failed");
    }
    if (recognizer.ComputeEmbedding(chip).empty()) {
        FACELOGIN_WARN(L"Warmup recognizer inference failed");
    }

    FACELOGIN_INFO(L"Inference warmup: %.1f ms",
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - start).count());
}

} // namespace facelogin
