#include "face_gain_tune.h"

#include <vector>

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
            [this](long v) { gain = v; gainWrites.push_back(v); return true; }
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

int main() {
    TestConsecutiveDarkUnlocks();
    TestMeasuredGainConvergesWithMargin();
    TestUnconfirmedProgressIsNotSaved();
    TestDarkFallbackAndBrightRecovery();
    std::printf("FaceGainTuneTest: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
