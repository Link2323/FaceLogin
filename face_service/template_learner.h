#pragma once

#include <windows.h>

#include "credential_store.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace facelogin {

// One candidate EMA update, produced by a fully successful authentication
// round (liveness 5/5 + 3-frame consensus + same-SID). Carries the
// best-distance binding frame of the round.
struct LearningSample {
    std::wstring sid;
    std::wstring username;          // logging only
    uint32_t faceId = 0;
    std::wstring faceLabel;         // logging only
    std::vector<float> embedding;   // unit-length probe embedding (owned copy)
    float distance = 0.0f;
    float norm = 0.0f;              // pre-normalization recognizer norm
    // Pose of the probe frame (weak-perspective kps estimate, ±90° physical
    // range; docs/progressive-learning-v2.md §3 pose-cone gate). The sentinel
    // matches credential_store.h kNominalAngleInvalid.
    float probeYawDeg = 1000.0f;
    float probePitchDeg = 1000.0f;
    // Landing clarity: how much closer the hit template is than the account's
    // second-nearest face (secondFaceDistance − distance). Huge = single-face
    // account (no wrong-slot risk). Frames closer than kClarityMargin to both
    // slots must not update either (equidistant → wrong-slot risk).
    float faceMargin = 1e9f;
    // p20 of this user's current-era successful auth distances (rolling
    // window, reset on RELOAD_DB = re-enrollment). Negative = window too
    // small; the effective gate then falls back to the configured cap.
    // Red line 1 (2026-09-03 revision): gate = min(userEraP20, cap).
    float userEraP20 = -1.0f;
    // Every comparable face of the authenticated account as seen by this
    // frame (id/label/nominal pose/distance), computed by the pipe thread
    // under the store lock. Drives the update-target selection: the V5
    // update goes to the slot whose nominal yaw is nearest the probe, which
    // with overlapping enrolled slots can differ from the distance-nearest
    // face (faceId above). Empty/invalid-nominal entries → legacy V4 path.
    std::vector<CredentialStore::AccountFaceDistance> accountFaces;
};

struct LearningConfig {
    bool enabled = true;
    float alpha = 0.10f;            // [0.05, 0.15], see config_util.h
    float distanceGate = 0.65f;     // [0.35, 0.65]
    float normFloor = 19.1f;        // [15.0, 25.0]
    int minIntervalSec = 60;        // [10, 3600]
    // Pose cone half-width: |probe − nominal| must stay below this on BOTH
    // axes for a V5 template (legacy V4 templates without a nominal angle
    // pass — no cone information, protected by distance+clarity instead).
    float coneHalfAngleDeg = 25.0f;
    // Landing-clarity threshold (LEGACY V4 PATH ONLY): with no nominal
    // angles the update target is still the distance-nearest face, and the
    // hit template must beat the account's second-nearest face by at least
    // this much. The V5 pose-selected path does not apply it — yaw owns slot
    // ownership and the distance gate is re-evaluated against the target.
    float clarityMargin = 0.10f;
};

// Observability snapshot backing the LEARNING_STATUS console query
// (FaceService assembles the JSON). In-memory only and cleared on service
// restart — the durable record stays the log. One event per PROCESSED
// sample: Enqueue's burst coalescing means rapid unlock bursts collapse
// into a single entry.
struct LearningEvent {
    std::chrono::system_clock::time_point time;
    std::wstring username;          // logging-grade, may be empty
    uint32_t faceId = 0;
    std::wstring faceLabel;
    bool accepted = false;
    const wchar_t* reason = nullptr;  // skip reason key; null when accepted
    float distance = 0.0f;
    float gate = 0.0f;                // effective gate evaluated for this sample
    float alphaUsed = 0.0f;           // accepted events only
};

struct LearningFaceStatus {
    std::wstring sid;
    std::wstring username;
    uint32_t faceId = 0;
    std::wstring faceLabel;
    unsigned long long acceptedTotal = 0;
    int acceptedToday = 0;
    int dayStamp = 0;               // internal acceptedToday rollover
    bool hadAccept = false;
    std::chrono::system_clock::time_point lastAccept{};
};

// Post-auth template EMA fine-tuning (docs/progressive-learning-v2.md §3).
//
// The pipe thread enqueues the round's best sample right after AUTH_SUCCESS
// delivery; this class owns everything after that, OFF the unlock critical
// path and OUT of the post-unlock busy window:
//   - the learner thread waits kStartDelayMs so Windows finishes the session
//     switch / shell startup before any learning work happens; rapid unlock
//     bursts naturally coalesce (min-distance sample of the burst wins),
//   - gate checks (distance / norm / interval / enabled) reject the sample
//     or pass it to BlendEMA,
//   - commits go through the Hooks so FaceService serializes all
//     CredentialStore access under its store lock (single-writer rule),
//   - disk writes are debounced: nothing flushes until kFlushQuietMs of
//     update silence, and a dirty state at shutdown flushes on Stop().
//
// Security invariants (docs §1): the update gate is far below the auth
// threshold so an impostor that barely passed authentication can never move
// a template — and the gate is evaluated against the pose-selected TARGET
// slot's distance, never the (smaller) matched-slot distance; liveness-failed
// rounds never reach Enqueue at all; the pose cone re-checks the target after
// selection; legacy V4 slots (no nominal angles) keep the distance-nearest
// target plus the landing-clarity gate; the store records (observation-only)
// the account's minimum template-pair distance on every commit; every
// accepted update keeps the previous embedding in a 3-generation ring and the
// first flush of a day writes users.dat.bak (see FaceService).
class TemplateLearner {
public:
    // What fetchTemplate returns for the target slot: the current embedding
    // plus its V5 nominal pose angles (invalid sentinel = legacy V4 record).
    struct TemplateSnapshot {
        std::vector<float> embedding;
        float nominalYaw = 1000.0f;
        float nominalPitch = 1000.0f;
    };

    struct Hooks {
        // Fetch the current stored embedding + nominal angles of (sid,
        // faceId). False = the face disappeared (e.g. re-enrolled between
        // round and processing).
        std::function<bool(const std::wstring& sid, uint32_t faceId,
                           TemplateSnapshot& out)> fetchTemplate;
        // Commit a blended embedding. The hook owns the store lock; false
        // means the target vanished and the sample is dropped (no retry).
        // Pair-spacing observation happens inside the store.
        std::function<bool(const LearningSample& sample,
                           const std::vector<float>& blended,
                           float alphaUsed)> commit;
        // Persist users.dat. Never called on the auth critical path.
        std::function<void()> flush;
    };

    TemplateLearner(LearningConfig config, Hooks hooks);
    ~TemplateLearner();

    TemplateLearner(const TemplateLearner&) = delete;
    TemplateLearner& operator=(const TemplateLearner&) = delete;

    // Pipe-thread call after AUTH_SUCCESS. Non-blocking: merges with a
    // pending sample (best distance wins) and wakes the learner thread.
    // Drops silently once stopped.
    void Enqueue(LearningSample sample);

    // CONFIG_RELOAD: swap gates; applies from the next sample on.
    void UpdateConfig(LearningConfig config);

    // Flush-if-dirty and join the learner thread. Idempotent; the destructor
    // calls it as the backstop.
    void Stop();

    // t' = L2-normalize(alpha * sample + (1 - alpha) * tmpl). Pure function,
    // public for unit tests and the poisoning-bound test.
    static std::vector<float> BlendEMA(const std::vector<float>& tmpl,
                                       const std::vector<float>& sample,
                                       float alpha);

    static constexpr int kStartDelayMs = 2000;   // post-unlock busy window
    static constexpr int kFlushQuietMs = 30000;  // update-driven flush debounce

    // LEARNING_STATUS snapshot. recent: newest first, at most maxCount
    // entries. Faces with counters: every (sid, faceId) that ever reached
    // ApplySample, regardless of accept/skip.
    std::vector<LearningEvent> RecentEvents(size_t maxCount) const;
    std::vector<LearningFaceStatus> FaceStatuses() const;

private:
    void Run();
    void ApplySample(const LearningSample& sample);
    void RecordEvent(const LearningSample& sample, float gate, bool accepted,
                     const wchar_t* reason, float alphaUsed);
    LearningConfig Config() const;

    mutable CRITICAL_SECTION m_cs{};
    LearningConfig m_config;   // guarded by m_cs
    Hooks m_hooks;

    HANDLE m_wake = nullptr;   // auto-reset event
    std::thread m_thread;
    std::atomic<bool> m_stopped{false};

    std::optional<LearningSample> m_pending;             // guarded by m_cs
    std::chrono::steady_clock::time_point m_pendingSince;

    std::chrono::system_clock::time_point m_lastAccept{};
    bool m_dirty = false;                                 // guarded by m_cs
    std::chrono::system_clock::time_point m_lastUpdate{};
    int m_dayCount = 0;                                   // α decay counter
    int m_dayStamp = 0;                                   // local yyyymmdd
    // Ring of previous embeddings for diagnostics/manual rollback support.
    struct Generation {
        std::wstring sid;
        uint32_t faceId = 0;
        std::vector<float> embedding;
    };
    std::deque<Generation> m_generations;

    // LEARNING_STATUS snapshot state (guarded by m_cs).
    static constexpr size_t kEventHistory = 32;
    std::deque<LearningEvent> m_events;   // newest back
    std::map<std::pair<std::wstring, uint32_t>, LearningFaceStatus> m_faceStatus;
};

} // namespace facelogin
