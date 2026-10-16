#pragma once

#include "auth_outcome.h"
#include "../vision/liveness_types.h"
#include "../vision/face_gain_tune.h"
#include "../vision/onnx_models.h"
#include "../common/frame_image.h"
#include "../common/ipc_protocol.h"

#include <functional>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace facelogin {

struct AuthFrame;
class AuthFrameStream;
namespace auth_diagnostics { struct NormLog; }

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
    // Sensor exposure/gain controls for the face-priority exposure tune
    // (face_gain_tune.h). Empty functions = unsupported knob → skipped.
    SensorKnobs sensorKnobs;
    // Optional diagnostic-only snapshot: a single driver read returns both
    // exposure value and mode. False means neither output is valid. Hosts
    // without it keep the separate SensorKnobs reads. Never used for tuning.
    std::function<bool(long& valueOut, bool& manualOut)> readExposureState;
    std::function<void(const std::wstring&)> reportStatus;

    // Called for each identity-binding frame (counted frames 1/3/5 in the
    // default plan; every counted frame in fast-unlock mode). The callback
    // must preserve the first-identity / same-identity invariant and must
    // not release a password. Retry means the current frame is not counted for PAD.
    // preNorm is the embedding's L2 norm before normalization (quality
    // signal; the embedding itself arrives unit-length so the raw norm is
    // unrecoverable downstream). yawDeg/pitchDeg are the weak-perspective
    // pose estimates from the same frame's keypoints (face_align.h) — the
    // progressive-learning pose-cone gate consumes them
    // (docs/work/in-progress/progressive-learning-v2.md §3).
    std::function<BindingDecision(const std::vector<float>&,
                                  unsigned int bindingIndex,
                                  float preNorm,
                                  float yawDeg,
                                  float pitchDeg)> verifyBinding;
};

struct AuthPipelineConfig {
    float antiSpoofThreshold = 0.28f;
    int authTimeoutSeconds = ipc::AUTH_TIMEOUT_SECONDS;
    int cameraRotation = 0;
    // Fast unlock: 3 counted frames, every frame binds identity (the
    // default 5-frame plan binds counted frames 1/3/5). Security
    // trade-off documented on AppConfig::fast_unlock.
    bool fastUnlock = false;
    // Absolute path of the persisted last-good exposure/gain combo
    // (camera_tune.state in the data dir, face_gain_tune.h). Empty disables
    // persistence and replay.
    std::wstring tuneStatePath;
    // Hosts pass their original deadline so preparation consumes the same budget.
    ipc::AuthDeadline authDeadline{};
};

struct AuthPipelineResult {
    AuthOutcome outcome = AuthOutcome::Failed;
    std::wstring errorMessage;
};

// How the counted-frame anti-spoof loop ended. Interrupted and
// ClientDisconnected have already written the terminal outcome (the
// interrupted lambda / Run's disconnect handling), so they must not go
// through the failure-message selection in ResolveOutcome.
enum class AuthLoopStop {
    CountingComplete,      // while-condition exit: every frame counted
    WindowExpired,         // 8 s PAD window elapsed
    EmptyScene,            // 2.5 s without any face
    Attack,                // 2 s persistent attack before the first PAD pass
    PadInferenceError,     // invalid PAD score — fail closed
    IdentityRejected,      // parent returned an explicit Reject
    IdentityRetryTimedOut, // 2 s of continuous identity misses
    Interrupted,           // cancellation or deadline (outcome already set)
    ClientDisconnected,    // credential provider went away (Run sets Cancelled)
};

// Verdict of the pre-count exposure gates: whether the triggering frame was
// consumed (sensor state and stream may have moved — grab fresh) or survives
// to the attack/binding stages.
enum class PreCountCorrection {
    NotNeeded,
    Corrected,
    Interrupted,
};

// Verdict of an identity-binding attempt on an anchor/consensus frame.
enum class BindingDisposition {
    Bound,        // binding accepted; frame proceeds to PAD counting
    Discard,      // embedding or identity miss — retry pace, frame not counted
    RetryTimedOut,// 2 s of continuous identity misses
    Rejected,     // terminal parent rejection
};

// Shared mutable state of the counted-frame loop and its stage helpers.
// Main-thread-only, like the loop itself.
struct AuthLoopState {
    explicit AuthLoopState(LivenessTiming::TimePoint windowStart) : timing(windowStart) {}

    LivenessTiming timing;
    int totalChecked = 0;
    unsigned int bindingCount = 0;
    bool havePrevRect = false;
    FaceRect prevRect{};
    // The parent verification callback returns Retry only for "no enrolled
    // identity matches this face". A face that keeps missing every enrolled
    // identity never leaves the anchor frame, so no frame is ever counted and
    // the persistent-attack timer fires without a single PAD verdict — that
    // outcome is an unknown face, not a PAD failure, and must not report
    // the misleading liveness message.
    bool sawUnknownFace = false;
    std::wstring identityError;
    // Capture conditions for the outcome log lines: the first identity
    // miss (what the unknown-face failure looked like) and the latest
    // accepted binding (what a passing frame looked like).
    long retryFaceWidth = 0;
    float retryFaceLuma = -1.0f;
    bool haveRetryConditions = false;
    long boundFaceWidth = 0;
    float boundFaceLuma = -1.0f;
    // Pre-binding exposure correction budget (2026-09-19 17:45:46
    // post-mortem): the pre-loop tune needs a face to measure, so a
    // trigger frame without one (late walk-up, dark-face miss) or
    // brightness that drifts after it left the loop matching a dark chip
    // until the attack timer killed the round — identity distance 0.989
    // at face luma 18. While NO frame has been counted yet, nothing
    // contributes to PAD or identity evidence, so sensor writes are still
    // safe; an out-of-band face luma earns one full tune pass (the gate
    // lives in CorrectExposureBeforeFirstCount, before the first binding
    // attempt). Two passes bound the worst case; a face that stays out of
    // band after them proceeds exactly like the legacy loop.
    int lateTuneBudget = 2;
    bool sensorKnobsWired = false;
};

struct PreparedAuthFrames;
namespace auth_diagnostics { struct ExposureTimingProfile; }

class AuthPipeline {
public:
    AuthPipeline(OnnxDetector& detector,
                 OnnxRecognizer& recognizer,
                 OnnxAntiSpoof& antiSpoof,
                 AuthPipelineConfig config,
                 AuthPipelineCallbacks callbacks);

    AuthPipelineResult Run();

private:
    bool PrepareFrames(PreparedAuthFrames& prepared,
                       auth_diagnostics::ExposureTimingProfile& exposureTiming,
                       const SensorKnobs& sensorKnobs,
                       const std::function<bool()>& interrupted,
                       const decltype(AuthPipelineCallbacks::grabFrame)& grabFrame);

    // Stage helpers Run is split into, by phase: loop gates, the pre-count
    // exposure corrections, identity binding, PAD counting, and the final
    // outcome mapping. All run on the camera's COM thread inside Run's
    // try block.
    std::optional<AuthLoopStop> CheckLoopGates(AuthLoopState& state,
                                               const std::function<bool()>& interrupted);
    AuthLoopStop RunLivenessLoop(AuthLoopState& state,
                                 AuthFrameStream& frames,
                                 PreparedAuthFrames& prepared,
                                 const SensorKnobs& sensorKnobs,
                                 const std::function<bool()>& interrupted,
                                 auth_diagnostics::NormLog& normLog);
    PreCountCorrection CorrectExposureBeforeFirstCount(
        AuthLoopState& state, PreparedAuthFrames& prepared, const AuthFrame& cur,
        const SensorKnobs& sensorKnobs, const std::function<bool()>& interrupted);
    BindingDisposition AttemptBinding(AuthLoopState& state, const AuthFrame& cur,
                                      const FaceRect& faceRect,
                                      std::future<float>& scoreFuture,
                                      auth_diagnostics::NormLog& normLog);
    std::optional<AuthLoopStop> PrefetchNextFrame(
        AuthLoopState& state, AuthFrameStream& frames, AuthFrame& next,
        std::future<float>& scoreFuture, int totalChecks,
        const std::function<bool()>& interrupted);
    std::optional<AuthLoopStop> CountPadFrame(AuthLoopState& state,
                                              std::future<float>& scoreFuture,
                                              const FaceRect& faceRect,
                                              const std::function<bool()>& interrupted);
    AuthPipelineResult ResolveOutcome(AuthLoopStop stop, const AuthLoopState& state) const;
    // Persists the round's knob-write progress when a FAILED round carries
    // face-backed evidence that it moved capture toward the band (see
    // EvaluateFailedRoundTuneProgress, face_gain_tune.h). Called at every
    // non-success terminal, including cancellation and preparation aborts.
    void PersistFailedRoundTuneProgress(const PreparedAuthFrames& prepared) const;

    OnnxDetector& m_detector;
    OnnxRecognizer& m_recognizer;
    OnnxAntiSpoof& m_antiSpoof;
    AuthPipelineConfig m_config;
    AuthPipelineCallbacks m_callbacks;
};

} // namespace facelogin
