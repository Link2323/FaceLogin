// Development-only test for TemplateLearner's pose-based update-target
// selection and gate chain (docs/progressive-learning-v2.md §3, 2026-09-05
// revision). Runs the real learner thread (kStartDelayMs applies), so each
// case takes ~2s; total ~20s.

#include "template_learner.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using facelogin::CredentialStore;
using facelogin::LearningConfig;
using facelogin::LearningEvent;
using facelogin::LearningSample;
using facelogin::TemplateLearner;

namespace {

constexpr size_t kDim = 512;
constexpr float kInvalidAngle = 1000.0f;  // kNominalAngleInvalid sentinel
int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

std::vector<float> Basis(size_t index) {
    std::vector<float> v(kDim, 0.0f);
    v[index] = 1.0f;
    return v;
}

// Unit vector a*e[i] + b*e[j], normalized.
std::vector<float> Mix(float a, size_t i, float b, size_t j) {
    std::vector<float> v(kDim, 0.0f);
    const float n = std::sqrt(a * a + b * b);
    v[i] = a / n;
    v[j] = b / n;
    return v;
}

float Dist(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0.0;
    for (size_t i = 0; i < kDim; i++) {
        const double d = a[i] - b[i];
        s += d * d;
    }
    return static_cast<float>(std::sqrt(s));
}

struct Harness {
    std::map<uint32_t, TemplateLearner::TemplateSnapshot> slots;
    std::vector<std::pair<uint32_t, std::vector<float>>> commits;
    std::unique_ptr<TemplateLearner> learner;

    Harness() {
        LearningConfig cfg;  // defaults: gate 0.65, cone 25, clarity 0.10
        cfg.minIntervalSec = 0;
        TemplateLearner::Hooks hooks;
        hooks.fetchTemplate = [this](const std::wstring&, uint32_t faceId,
                                     TemplateLearner::TemplateSnapshot& out) {
            auto it = slots.find(faceId);
            if (it == slots.end()) return false;
            out = it->second;
            return true;
        };
        hooks.commit = [this](const LearningSample& s,
                              const std::vector<float>& blended, float) {
            commits.emplace_back(s.faceId, blended);
            return true;
        };
        hooks.flush = [] {};
        learner = std::make_unique<TemplateLearner>(cfg, std::move(hooks));
    }
    ~Harness() { learner->Stop(); }

    void AddSlot(uint32_t id, const wchar_t* label, float yaw,
                 const std::vector<float>& emb) {
        TemplateLearner::TemplateSnapshot s;
        s.embedding = emb;
        s.nominalYaw = yaw;
        s.nominalPitch = 0.0f;
        slots[id] = s;
        slotLabels[id] = label;
    }
    std::map<uint32_t, std::wstring> slotLabels;

    // Enqueue one sample and wait for its event (the learner thread delays
    // kStartDelayMs). Returns the newest event.
    LearningEvent Run(const LearningSample& sample) {
        const size_t before = learner->RecentEvents(64).size();
        learner->Enqueue(sample);
        for (int waited = 0; waited < 150; ++waited) {  // 15s cap
            Sleep(100);
            if (learner->RecentEvents(64).size() > before) break;
        }
        auto events = learner->RecentEvents(2);
        Check(events.size() >= 1 && events[0].username == sample.username,
              "learner produced the enqueued event");
        return events[0];  // newest first
    }
};

LearningSample MakeSample(const std::vector<float>& probe, float yaw,
                          uint32_t matchedFaceId, float matchedDist) {
    LearningSample s;
    s.sid = L"S-TEST";
    s.username = L"tester";
    s.faceId = matchedFaceId;
    s.faceLabel = L"正面";
    s.embedding = probe;
    s.distance = matchedDist;
    s.norm = 22.0f;
    s.probeYawDeg = yaw;
    s.probePitchDeg = 0.0f;
    s.faceMargin = 1e9f;
    s.userEraP20 = -1.0f;
    return s;
}

void FillAccountFaces(LearningSample& s, const Harness& h,
                      const std::vector<float>& probe) {
    for (const auto& [id, snap] : h.slots) {
        CredentialStore::AccountFaceDistance f;
        f.faceId = id;
        f.label = h.slotLabels.at(id);
        f.nominalYaw = snap.nominalYaw;
        f.nominalPitch = snap.nominalPitch;
        f.distance = Dist(probe, snap.embedding);
        s.accountFaces.push_back(f);
    }
}

}  // namespace

int main() {
    // 1. Pose selection overrides distance-nearest: probe is EMBEDDING-nearest
    //    to frontal but its yaw (+20) targets the 左转 slot (overlapping-slot
    //    scenario); the update must land on the pose slot.
    {
        Harness h;
        const auto frontal = Basis(0);
        const auto left = Mix(0.95f, 0, 0.312f, 1);   // d(frontal) ~ 0.32
        h.AddSlot(1, L"正面", 0.0f, frontal);
        h.AddSlot(2, L"左转", 30.0f, left);
        const auto probe = Mix(0.995f, 0, 0.10f, 1);  // nearer frontal
        Check(Dist(probe, frontal) < Dist(probe, left),
              "case1 precondition: frontal is distance-nearest");
        LearningSample s = MakeSample(probe, 20.0f, 1, Dist(probe, frontal));
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(ev.accepted, "case1: pose-selected update accepted");
        Check(ev.faceId == 2 && ev.faceLabel == L"左转",
              "case1: event attributed to yaw-nearest slot");
        Check(h.commits.size() == 1 && h.commits[0].first == 2,
              "case1: commit went to the yaw-nearest slot");
        Check(std::fabs(ev.distance - Dist(probe, left)) < 0.01f,
              "case1: event distance is the TARGET-slot distance");
    }

    // 2. The distance gate is evaluated against the pose-selected target, not
    //    the matched slot: probe 0.05 from frontal would pass the old gate,
    //    but the yaw target (orthogonal 左转 template) is 1.41 away → reject.
    {
        Harness h;
        h.AddSlot(1, L"正面", 0.0f, Basis(0));
        h.AddSlot(2, L"左转", 30.0f, Basis(1));
        const auto probe = Mix(1.0f, 0, 0.0f, 1);
        LearningSample s = MakeSample(probe, 20.0f, 1, 0.05f);
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(!ev.accepted && ev.reason && wcscmp(ev.reason, L"distance") == 0,
              "case2: rejected by the distance gate");
        Check(ev.distance > 1.0f,
              "case2: event distance reflects the far TARGET slot");
        Check(h.commits.empty(), "case2: nothing committed");
    }

    // 3. Exact yaw tie (probe +15 between nominals 0 and +30) breaks toward
    //    the smaller embedding distance.
    {
        Harness h;
        const auto frontal = Basis(0);
        const auto left = Mix(0.95f, 0, 0.312f, 1);
        h.AddSlot(1, L"正面", 0.0f, frontal);
        h.AddSlot(2, L"左转", 30.0f, left);
        const auto probe = Mix(0.99f, 0, 0.141f, 1);
        Check(Dist(probe, frontal) < Dist(probe, left),
              "case3 precondition: frontal closer for the tie-break");
        LearningSample s = MakeSample(probe, 15.0f, 1, Dist(probe, frontal));
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(ev.accepted && ev.faceId == 1,
              "case3: yaw tie broke toward smaller distance (frontal)");
    }

    // 4. A probe beyond 25° of EVERY nominal (yaw +60 vs 0/±30 slots) is
    //    cone-rejected even though all slots are embedding-close.
    {
        Harness h;
        h.AddSlot(1, L"正面", 0.0f, Mix(1.0f, 0, 0.0f, 1));
        h.AddSlot(2, L"左转", 30.0f, Mix(0.999f, 0, 0.0447f, 1));
        h.AddSlot(3, L"右转", -30.0f, Mix(0.999f, 0, -0.0447f, 1));
        const auto probe = Mix(1.0f, 0, 0.0f, 1);
        LearningSample s = MakeSample(probe, 60.0f, 1, 0.0f);
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(!ev.accepted && ev.reason && wcscmp(ev.reason, L"cone") == 0,
              "case4: yaw beyond every nominal cone-rejected");
    }

    // 5. The pitch axis of the cone still applies on the pose path.
    {
        Harness h;
        h.AddSlot(1, L"正面", 0.0f, Mix(1.0f, 0, 0.0f, 1));
        h.AddSlot(2, L"左转", 30.0f, Mix(0.999f, 0, 0.0447f, 1));
        const auto probe = Mix(1.0f, 0, 0.0f, 1);
        LearningSample s = MakeSample(probe, 10.0f, 1, 0.0f);
        s.probePitchDeg = 40.0f;
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(!ev.accepted && ev.reason && wcscmp(ev.reason, L"cone") == 0,
              "case5: pitch beyond cone rejected");
    }

    // 6. Legacy V4 path (no nominal angles): the landing-clarity gate still
    //    blocks equidistant frames.
    {
        Harness h;
        h.AddSlot(1, L"脸1", kInvalidAngle, Mix(1.0f, 0, 0.0f, 1));
        h.AddSlot(2, L"脸2", kInvalidAngle, Mix(0.999f, 0, 0.0447f, 1));
        const auto probe = Mix(1.0f, 0, 0.0f, 1);
        LearningSample s = MakeSample(probe, 0.0f, 1, 0.05f);
        s.faceMargin = 0.02f;
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(!ev.accepted && ev.reason && wcscmp(ev.reason, L"clarity") == 0,
              "case6: V4 equidistant frame clarity-blocked");
        Check(h.commits.empty(), "case6: nothing committed");
    }

    // 7. Legacy V4 path with a clear margin commits to the matched slot and
    //    skips the cone (invalid nominals).
    {
        Harness h;
        h.AddSlot(1, L"脸1", kInvalidAngle, Mix(1.0f, 0, 0.0f, 1));
        h.AddSlot(2, L"脸2", kInvalidAngle, Mix(0.95f, 0, 0.312f, 1));
        const auto probe = Mix(1.0f, 0, 0.0f, 1);
        LearningSample s = MakeSample(probe, 0.0f, 1, 0.0f);
        s.faceMargin = 0.40f;
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(ev.accepted && ev.faceId == 1,
              "case7: V4 clear-margin frame committed to matched slot");
    }

    // 8. Mixed account: a V4-era face is distance-nearest, but a V5 face with
    //    a valid nominal wins the pose selection.
    {
        Harness h;
        h.AddSlot(1, L"脸1", kInvalidAngle, Mix(1.0f, 0, 0.0f, 1));
        h.AddSlot(2, L"左转", 30.0f, Mix(0.95f, 0, 0.312f, 1));
        const auto probe = Mix(0.995f, 0, 0.10f, 1);
        Check(Dist(probe, h.slots[1].embedding) < Dist(probe, h.slots[2].embedding),
              "case8 precondition: V4 face is distance-nearest");
        LearningSample s = MakeSample(probe, 20.0f, 1, Dist(probe, h.slots[1].embedding));
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(ev.accepted && ev.faceId == 2,
              "case8: V5 face won pose selection over nearer V4 face");
    }

    // 9. Invalid probe pose estimate (sentinel) falls back to the legacy
    //    distance-nearest path.
    {
        Harness h;
        h.AddSlot(1, L"正面", 0.0f, Mix(1.0f, 0, 0.0f, 1));
        h.AddSlot(2, L"左转", 30.0f, Mix(0.95f, 0, 0.312f, 1));
        const auto probe = Mix(0.995f, 0, 0.10f, 1);
        LearningSample s = MakeSample(probe, kInvalidAngle, 1, 0.10f);
        s.faceMargin = 0.50f;
        FillAccountFaces(s, h, probe);
        LearningEvent ev = h.Run(s);
        Check(ev.accepted && ev.faceId == 1,
              "case9: invalid pose estimate fell back to distance path");
    }

    if (g_failures == 0) {
        std::printf("TemplateLearnerTest: all checks passed\n");
        return 0;
    }
    std::printf("TemplateLearnerTest: %d check(s) failed\n", g_failures);
    return 1;
}
