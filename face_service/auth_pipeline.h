#pragma once

#include "liveness_types.h"
#include "onnx_models.h"
#include "../common/frame_image.h"

#include <functional>
#include <string>
#include <vector>

namespace facelogin {

// The inference-only authentication loop.  It owns no camera, credential
// store, named pipe, or Windows password.  Those resources deliberately live
// behind callbacks so this exact PAD / identity-binding algorithm can execute
// in the short-lived authentication worker while the parent service retains
// credentials and the public Credential Provider pipe.
enum class BindingDecisionKind {
    Accept,
    Retry,
    Reject,
};

struct BindingDecision {
    BindingDecisionKind kind = BindingDecisionKind::Retry;
    std::wstring errorMessage;  // only used for Reject
};

struct AuthPipelineCallbacks {
    // Returns one raw RGB camera frame. frameSequence receives the camera's
    // monotonic frame counter for that grab; identical consecutive values
    // mean the same buffered frame was returned again (the exposure warmup
    // samples only distinct frames). AuthPipeline applies cameraRotation
    // immediately after a successful grab so every stage sees the same image.
    std::function<bool(FrameImage&, unsigned long long& frameSequence)> grabFrame;
    std::function<bool()> isCancelled;
    std::function<bool()> isClientDisconnected;
    std::function<void(const std::wstring&)> reportStatus;

    // Called for binding frames 1/3/5. The callback must preserve the
    // first-identity / same-identity invariant and must not release a
    // password. Retry means the current frame is not counted for PAD.
    // preNorm is the embedding's L2 norm before normalization (quality
    // signal; the embedding itself arrives unit-length so the raw norm is
    // unrecoverable downstream).
    std::function<BindingDecision(const std::vector<float>&,
                                  unsigned int bindingIndex,
                                  float preNorm)> verifyBinding;
};

struct AuthPipelineConfig {
    float antiSpoofThreshold = 0.28f;
    int authTimeoutSeconds = 15;
    int cameraRotation = 0;
};

struct AuthPipelineResult {
    bool succeeded = false;
    bool cancelled = false;
    bool timedOut = false;
    std::wstring errorMessage;
};

class AuthPipeline {
public:
    AuthPipeline(OnnxDetector& detector,
                 OnnxRecognizer& recognizer,
                 OnnxAntiSpoof& antiSpoof,
                 AuthPipelineConfig config,
                 AuthPipelineCallbacks callbacks);

    AuthPipelineResult Run();

private:
    OnnxDetector& m_detector;
    OnnxRecognizer& m_recognizer;
    OnnxAntiSpoof& m_antiSpoof;
    AuthPipelineConfig m_config;
    AuthPipelineCallbacks m_callbacks;
};

} // namespace facelogin
