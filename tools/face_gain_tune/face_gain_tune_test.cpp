#include "face_gain_tune.h"

#include <vector>
#include <fstream>
#include <sstream>

using namespace facelogin;

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}

struct Sensor {
    long exposure = -4;
    long gain = 50;
    std::vector<long> gainWrites;
    SensorKnobs Knobs() {
        return {
            [](long& lo, long& hi, long& step) { lo = -7; hi = -1; step = 1; return true; },
            [this](long& v) { v = exposure; return true; },
            [this](long v) { exposure = v; return true; },
            [](long& lo, long& hi, long& step) { lo = 0; hi = 63; step = 1; return true; },
            [this](long& v) { v = gain; return true; },
            [this](long v) { gain = v; gainWrites.push_back(v); return true; },
            [](bool& manual) { manual = true; return true; }
        };
    }
    float Luma() const { return 27.0f + static_cast<float>(gain - 50) * 2.5f; }
};

struct StateFile {
    std::wstring path;
    StateFile() {
        wchar_t dir[MAX_PATH]{}, file[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, dir) && GetTempFileNameW(dir, L"flt", 0, file)) path = file;
        Check(!path.empty(), "create temporary state file");
    }
    ~StateFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
};

GainTuneConfig Config(const StateFile& file) {
    GainTuneConfig cfg;
    cfg.persistPath = file.path;
    cfg.settleMs = 0; // deterministic measurements; production keeps its 900 ms floor
    return cfg;
}
const auto noCancel = [] { return false; };

void TestConsecutiveDarkUnlocks() {
    StateFile file;
    Sensor sensor;
    const auto knobs = sensor.Knobs();
    auto cfg = Config(file);
    cfg.maxSteps = 1; // force a budget-limited off-band checkpoint
    Check(SaveTuneKnobState(file.path, knobs), "seed old in-band state");
    const auto measure = [&](float& luma) { luma = sensor.Luma(); return true; };
    Check(TuneFaceExposure(sensor.Luma(), knobs, measure, noCancel, cfg) == 1,
          "first dark round exhausts its step budget");
    Check(sensor.gain == 55 && sensor.Luma() < 50, "first round ends below target at gain 55");
    TuneKnobState saved;
    Check(LoadTuneKnobState(file.path, saved) && saved.gain == 55,
          "off-band settled progress replaces stale gain 50");
    sensor.gainWrites.clear();
    Check(!ApplyTuneKnobState(knobs, saved) && sensor.gainWrites.empty(),
          "next graph preserves progress without rolling back");
    cfg.maxSteps = 3;
    Check(TuneFaceExposure(sensor.Luma(), knobs, measure, noCancel, cfg) <= 2,
          "second round continues progress and reaches target within two steps");
    Check(sensor.gain > 55 && sensor.Luma() >= 56, "second round converges with margin");
    const long convergedGain = sensor.gain;
    Check(LoadTuneKnobState(file.path, saved) && saved.gain == convergedGain, "converged state saved");
    Check(!ApplyTuneKnobState(knobs, saved), "third graph does not rewrite intact controls");
    Check(TuneFaceExposure(sensor.Luma(), knobs, measure, noCancel, cfg) == 0,
          "third round needs no tuning");
    sensor.gain = 0;
    sensor.exposure = -5;
    Check(ApplyTuneKnobState(knobs, saved) && sensor.gain == convergedGain && sensor.exposure == -4,
          "power loss restores the latest converged state");
}

void TestMeasuredGainConvergesWithMargin() {
    // Approximate the user's 11:41 trace: gain 50 gives luma 25, gain 58
    // gives 46, gain 60 is borderline, gain 62 is stable around 55.
    // The old controller needed five writes spread over three unlocks.
    for (float slope : {2.4f, 4.0f}) {
        StateFile file;
        Sensor sensor;
        const auto knobs = sensor.Knobs();
        auto cfg = Config(file);
        const auto lumaNow = [&] { return 25.0f + (sensor.gain - 50) * slope; };
        const auto measure = [&](float& luma) { luma = lumaNow(); return true; };
        Check(TuneFaceExposure(lumaNow(), knobs, measure, noCancel, cfg) == 2,
              "settled response converges in two gain writes in one round");
        Check(sensor.exposure == -4 && sensor.gain <= 63,
              "gain prediction respects physical range and avoids slower exposure");
        Check(lumaNow() >= 56 && lumaNow() <= 90, "gain lands inside the band with margin");
        sensor.gainWrites.clear();
        Check(TuneFaceExposure(lumaNow() - 5.0f, knobs, measure, noCancel, cfg) == 0 &&
              sensor.gainWrites.empty(), "small next-round brightness drop does not re-trigger");
    }
}

void TestUnconfirmedProgressIsNotSaved() {
    // Lost face, cancellation, oscillation and exhausted sample budget before
    // the time floor must all preserve the previous checkpoint.
    for (int scenario = 0; scenario < 4; ++scenario) {
        StateFile file;
        Sensor sensor;
        const auto knobs = sensor.Knobs();
        auto cfg = Config(file);
        cfg.settleFrames = 4;
        if (scenario == 3) cfg.settleMs = 60000;
        Check(SaveTuneKnobState(file.path, knobs), "seed checkpoint before failed measurement");
        int samples = 0;
        bool cancelled = false;
        const auto measure = [&](float& luma) {
            ++samples;
            if (scenario == 0) return false;
            if (scenario == 1) cancelled = true;
            luma = scenario == 2 ? (samples % 2 ? 10.0f : 100.0f) : 45.0f;
            return true;
        };
        TuneFaceExposure(27.0f, knobs, measure, [&] { return cancelled; }, cfg);
        TuneKnobState saved;
        Check(LoadTuneKnobState(file.path, saved) && saved.gain == 50,
              "unconfirmed/cancelled write does not replace checkpoint");
        Check(sensor.gainWrites.size() == 1, "no further writes after unconfirmed measurement");
    }
}

void TestDarkFallbackAndBrightRecovery() {
    StateFile file;
    Sensor sensor;
    const auto knobs = sensor.Knobs();
    auto cfg = Config(file);
    sensor.gain = 63;
    auto darkMeasure = [&](float& luma) {
        luma = 20.0f * std::pow(2.0f, static_cast<float>(sensor.exposure + 4));
        return true;
    };
    Check(TuneFaceExposure(20.0f, knobs, darkMeasure, noCancel, cfg) > 0 && sensor.exposure > -4,
          "exhausted gain still permits existing extreme-dark exposure fallback");
    TuneKnobState saved;
    Check(LoadTuneKnobState(file.path, saved) && saved.exposure == sensor.exposure,
          "fallback persists settled exposure");
    const long before = sensor.exposure;
    auto brightMeasure = [&](float& luma) {
        luma = 170.0f * std::pow(2.0f, static_cast<float>(sensor.exposure - before));
        return true;
    };
    Check(TuneFaceExposure(170.0f, knobs, brightMeasure, noCancel, cfg) == 1 && sensor.exposure < before,
          "persisted dark-room state still adjusts down when room becomes bright");
}
} // namespace

void TestWhiteoutEvidence() {
    FrameImage frame(100, 100);
    Check(!IsSceneWhiteout(frame) && !IsSceneWhiteout(FrameImage{}),
          "dark and empty frames do not trigger recovery");
    for (long y = 0; y < frame.nr(); ++y)
        for (long x = 0; x < frame.nc(); ++x) frame(y, x) = RgbPixel(255, 255, 255);
    Check(IsSceneWhiteout(frame), "widespread clipping triggers recovery evidence");
    for (long y = 40; y < 60; ++y)
        for (long x = 40; x < 60; ++x) frame(y, x) = RgbPixel(30, 30, 30);
    Check(!IsSceneWhiteout(frame), "bright background with dark central subject is excluded");
    for (long y = 0; y < frame.nr(); ++y)
        for (long x = 0; x < frame.nc(); ++x) frame(y, x) = RgbPixel(230, 230, 230);
    Check(!IsSceneWhiteout(frame), "bright but unclipped empty scene is excluded");
}

void TestNoFaceRecovery() {
    StateFile file;
    Sensor sensor;
    auto knobs = sensor.Knobs();
    auto cfg = Config(file);
    Check(SaveTuneKnobState(file.path, knobs), "save pre-recovery checkpoint");
    float recovered = -1.0f;
    int samples = 0;
    auto measure = [&](WhiteoutSample& sample) {
        ++samples;
        sample = {sensor.exposure > -6, sensor.exposure <= -6 ? 80.0f : -1.0f};
        return true;
    };
    Check(RecoverSceneWhiteout(knobs, measure, noCancel, recovered, cfg) == 2 &&
          sensor.exposure == -6 && recovered == 80.0f && samples >= 6,
          "no-face clipping reduces exposure until settled detection returns");
    TuneKnobState saved;
    Check(LoadTuneKnobState(file.path, saved) && saved.exposure == -4,
          "recovery alone never overwrites the face checkpoint");
    Check(TuneFaceExposure(recovered, knobs, [](float&) { return false; }, noCancel, cfg) == 0 &&
          LoadTuneKnobState(file.path, saved) && saved.exposure == -6,
          "settled face handoff can persist the recovered in-band settings");

    sensor.exposure = -4;
    samples = 0;
    auto transient = [&](WhiteoutSample& sample) {
        sample = {++samples == 1, -1.0f}; return true;
    };
    Check(RecoverSceneWhiteout(knobs, transient, noCancel, recovered, cfg) == 0 &&
          sensor.exposure == -4, "single clipped frame cannot trigger a write");

    auto clipped = [](WhiteoutSample& sample) { sample = {true, -1.0f}; return true; };
    cfg.maxSteps = 2;
    Check(RecoverSceneWhiteout(knobs, clipped, noCancel, recovered, cfg) == 2 &&
          sensor.exposure == -6 && recovered < 0,
          "persistent clipping has a strict write budget and no face handoff");
    sensor.exposure = -7;
    Check(RecoverSceneWhiteout(knobs, clipped, noCancel, recovered, cfg) == 0,
          "exposure lower bound is respected");
    sensor.exposure = -4;
    Check(RecoverSceneWhiteout(knobs, clipped, [] { return true; }, recovered, cfg) == 0,
          "cancellation before recovery writes nothing");
    Check(RecoverSceneWhiteout(knobs, clipped, [&] { return sensor.exposure < -4; },
                              recovered, cfg) == 1 && recovered < 0,
          "cancellation after a write stops without handoff");
    sensor.exposure = -4;
    Check(RecoverSceneWhiteout(knobs, [](WhiteoutSample&) { return false; },
                              noCancel, recovered, cfg) == 0,
          "dead camera does not change controls");
    auto failedAfterWrite = [&](WhiteoutSample& sample) {
        sample = {true, -1.0f}; return sensor.exposure == -4;
    };
    Check(RecoverSceneWhiteout(knobs, failedAfterWrite, noCancel, recovered, cfg) == 1 &&
          recovered < 0, "lost camera after write prevents another write or handoff");
    sensor.exposure = -4;
    cfg.settleMs = 900;
    cfg.settleFrames = 2;
    Check(RecoverSceneWhiteout(knobs, clipped, noCancel, recovered, cfg) == 0,
          "sample agreement before actuator lag expires cannot authorize a write");
    cfg.settleMs = 0;
    cfg.settleFrames = 4;
    samples = 0;
    auto unstableAfterWrite = [&](WhiteoutSample& sample) {
        sample = {true, sensor.exposure < -4 ? (++samples % 2 ? 80.0f : 160.0f) : -1.0f};
        return true;
    };
    Check(RecoverSceneWhiteout(knobs, unstableAfterWrite, noCancel, recovered, cfg) == 1 &&
          recovered < 0, "unstable face readings prevent handoff and stacked writes");
    sensor.exposure = -4;
    knobs.exposureSet = {};
    Check(RecoverSceneWhiteout(knobs, clipped, noCancel, recovered, cfg) == 0,
          "unsupported exposure control is a no-op");
}

void TestResponseGate() {
    GainTuneConfig cfg;
    detail::FaceSettleGate fast(cfg, 30, 1);
    for (int ms = 0; ms <= 120; ms += 40)
        Check(!fast.Feed(30, ms), "unchanged old frames cannot complete early");
    for (int ms = 160; ms < 360; ms += 40)
        Check(!fast.Feed(60, ms), "directional response needs a full plateau");
    Check(fast.Feed(60, 360), "real response and 200ms plateau finish before 900ms");
    detail::FaceSettleGate delayed(cfg, 30, 1);
    for (int ms = 0; ms < 900; ms += 50)
        Check(!delayed.Feed(30, ms), "long stale plateau is not response evidence");
    Check(delayed.Feed(30, 900), "no-response path retains original 900ms gate");
    detail::FaceSettleGate transient(cfg, 30, 1);
    Check(!transient.Feed(60, 100) && !transient.Feed(30, 150), "flash resets plateau");
    for (int ms = 200; ms < 900; ms += 50)
        Check(!transient.Feed(30, ms), "old frames after flash cannot finish early");
    detail::FaceSettleGate wrong(cfg, 60, -1);
    for (int ms = 0; ms < 900; ms += 50)
        Check(!wrong.Feed(80, ms), "opposite-direction lighting change is not actuator evidence");
    detail::FaceSettleGate ramp(cfg, 30, 1);
    for (int ms = 0; ms < 900; ms += 40)
        Check(!ramp.Feed(40.0f + ms / 20.0f, ms), "ongoing ramp does not satisfy plateau");
    detail::FaceSettleGate slowDrift(cfg, 30, 1);
    for (int ms = 0; ms < 1300; ms += 40)
        Check(!slowDrift.Feed(60.0f - ms * 0.0035f, ms),
              "small-range drift stays unconfirmed even after the 900ms fallback");
}

void TestShortExposurePolicy() {
    StateFile file;
    Sensor sensor;
    auto knobs = sensor.Knobs();
    auto cfg = Config(file);
    sensor.exposure = -6;
    const auto gainOnly = [&](float& luma) { luma = sensor.Luma(); return true; };
    Check(TuneFaceExposure(27, knobs, gainOnly, noCancel, cfg) <= 2 && sensor.exposure == -6,
          "dark face uses responsive gain without sacrificing short exposure");
    sensor.gain = 50;
    cfg.maxSteps = 1;
    Check(TuneFaceExposure(27, knobs, gainOnly, noCancel, cfg) == 1 && sensor.exposure == -6,
          "gain budget exhaustion is not evidence that longer exposure is necessary");
    cfg.maxSteps = 3;
    sensor.exposure = -4;
    const auto bright = [&](float& luma) {
        luma = 118.0f * std::exp2(static_cast<float>(sensor.exposure + 4)); return true;
    };
    Check(TuneFaceExposure(118, knobs, bright, noCancel, cfg) == 1 && sensor.exposure == -5,
          "in-band but slow 62.5ms exposure shortens to 31.25ms with margin");
    Check(TuneFaceExposure(59, knobs, bright, noCancel, cfg) == 0,
          "next authentication reuses short exposure without another search");
    sensor.exposure = -3;
    Check(TuneFaceExposure(80, knobs, bright, noCancel, cfg) == 0 && sensor.exposure == -3,
          "insufficient light margin does not speculate with a darker exposure");
    sensor.exposure = -5;
    Check(TuneFaceExposure(118, knobs, bright, noCancel, cfg) == 0,
          "already 30fps-compatible exposure avoids needless shorter probes");
    sensor.exposure = -8;
    sensor.gain = 63;
    auto photon = [&](float& luma) {
        luma = 30.0f * std::exp2(static_cast<float>(sensor.exposure + 8)); return true;
    };
    // Extend the mock range to test that a dark face does not jump -8 -> -4.
    knobs.exposureRange = [](long& lo, long& hi, long& step) { lo=-10; hi=-1; step=1; return true; };
    Check(TuneFaceExposure(30, knobs, photon, noCancel, cfg) == 1 && sensor.exposure == -7,
          "gain exhaustion adds only the one exposure stop actually needed");
    sensor.exposure = -5;
    sensor.gain = 50;
    bool manual = false;
    int pins = 0;
    knobs.exposureIsManual = [&](bool& out) { out=manual; return true; };
    knobs.exposureSet = [&](long v) { sensor.exposure=v; manual=true; ++pins; return true; };
    knobs.gainSet = [&](long v) { Check(manual, "gain must never race AE"); sensor.gain=v; return true; };
    Check(TuneFaceExposure(27, knobs, gainOnly, noCancel, cfg) >= 2 && pins == 1 && sensor.exposure == -5,
          "automatic exposure is pinned at its current short value before gain");
    manual = false;
    sensor.gain = 50;
    knobs.exposureSet = [](long) { return false; };
    Check(TuneFaceExposure(27, knobs, gainOnly, noCancel, cfg) == 0 && sensor.gain == 50,
          "failed manual takeover cannot start gain tuning against AE");
    knobs = sensor.Knobs();
    sensor.exposure = -6;
    sensor.gain = 0;
    sensor.gainWrites.clear();
    const auto deadGain = [&](float& luma) {
        luma = 30.0f * std::exp2(static_cast<float>(sensor.exposure + 6)); return true;
    };
    Check(TuneFaceExposure(30, knobs, deadGain, noCancel, cfg) == 3 &&
          sensor.gainWrites.size() == 2 && sensor.exposure == -5,
          "dead gain gets no repeated second pass and exposure recovers in one stop");
}

// Optional replay of ExposureResponseProbe CSV through the actual gate.
void ReplayResponseCsv(const char* path) {
    std::ifstream input(path);
    Check(input.good(), "open real-camera response trace");
    std::string line, key;
    std::vector<std::pair<long long,float>> samples;
    float before = -1;
    int direction = 0;
    int groups = 0;
    const auto flush = [&] {
        if (samples.empty()) return;
        ++groups;
        std::vector<float> tail;
        for (size_t i = samples.size() > 10 ? samples.size()-10 : 0; i < samples.size(); ++i)
            tail.push_back(samples[i].second);
        std::sort(tail.begin(), tail.end());
        const float final = tail[tail.size()/2];
        if (before >= 0) {
            GainTuneConfig cfg;
            detail::FaceSettleGate gate(cfg, before, direction);
            bool accepted = false;
            for (size_t i = 0; i < samples.size() && i < static_cast<size_t>(cfg.settleFrames); ++i) {
              auto [ms,luma] = samples[i];
              if (gate.Feed(luma,ms)) {
                std::printf("TRACE %s gate=%lldms accepted=%.1f tail=%.1f drift=%.1f\n",
                            key.c_str(),ms,luma,final,luma-final);
                accepted = true;
                break;
              }
            }
            if (!accepted) std::printf("TRACE %s unconfirmed within sample budget\n", key.c_str());
        }
        before=final;
        samples.clear();
    };
    std::getline(input,line);
    while (std::getline(input,line)) {
        std::stringstream row(line);
        std::vector<std::string> cells;
        std::string cell;
        while(std::getline(row,cell,',')) cells.push_back(cell);
        if(cells.size()!=8) continue;
        const std::string next=cells[0]+"/"+cells[1];
        if(next!=key) { flush(); key=next; direction=std::stoi(cells[7]); }
        samples.push_back({static_cast<long long>(std::stod(cells[2])),std::stof(cells[5])});
    }
    flush();
    Check(groups > 1, "response trace contains baseline and actuator measurements");
}

int main(int argc, char** argv) {
    TestResponseGate();
    TestShortExposurePolicy();
    if(argc==2) ReplayResponseCsv(argv[1]);
    TestWhiteoutEvidence();
    TestNoFaceRecovery();
    TestConsecutiveDarkUnlocks();
    TestMeasuredGainConvergesWithMargin();
    TestUnconfirmedProgressIsNotSaved();
    TestDarkFallbackAndBrightRecovery();
    std::printf("FaceGainTuneTest: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
