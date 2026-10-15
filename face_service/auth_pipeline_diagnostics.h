#pragma once

#include "../vision/face_gain_tune.h"
#include "../common/logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace facelogin::auth_diagnostics {

// Main-thread-only, per-request diagnostics. Control reads remain fresh;
// timing does not cache results or change callback arguments/return values.
struct ExposureTimingProfile {
    enum Phase { Replay, ReplaySettle, Trigger, Tune, Loop, PhaseCount };
    enum Operation { ExposureRange, ExposureGet, ExposureSet, GainRange,
                     GainGet, GainSet, ExposureMode, ExposureAuto, ExposureState,
                     OperationCount };
    struct Calls {
        unsigned int count = 0;
        unsigned int failed = 0;
        double totalMs = 0.0;
        double maxMs = 0.0;
    };
    std::array<std::array<Calls, OperationCount>, PhaseCount> calls{};
    Phase phase = Replay;
    bool gateReached = false;
    double preparationMs = 0.0;
    double seedWaitMs = 0.0;
    double seedRefreshMs = 0.0;
    double replaySettleMs = 0.0;
    double triggerMs = 0.0;
    double triggerQueryMs = 0.0;
    bool triggerQueriesBeforeSeedWait = false;
    bool reusedCompletedSeed = false;
    unsigned long long warmupSeedSeq = 0;
    double warmupSeedAgeMs = -1.0;
    double tuneMs = 0.0;
    double statusMs = 0.0;
    double gateToLoopMs = 0.0;

    ~ExposureTimingProfile() {
        // Emit only after the work, so measuring individual driver calls does
        // not add a log write between them or perturb the warmup schedule.
        if (gateReached) {
            FACELOGIN_INFO(L"Exposure phase timing: preparation=%.2f ms, seed_wait=%.2f ms, seed_refresh=%.2f ms, "
                           L"replay_settle=%.2f ms, trigger=%.2f ms, tune=%.2f ms, "
                           L"status=%.2f ms, gate_to_loop=%.2f ms, "
                           L"trigger_query=%.2f ms, query_before_seed_wait=%d, "
                           L"seed_reused=%d, seed_seq=%llu, seed_age=%.2f ms",
                           preparationMs, seedWaitMs, seedRefreshMs, replaySettleMs, triggerMs,
                           tuneMs, statusMs, gateToLoopMs, triggerQueryMs,
                           triggerQueriesBeforeSeedWait, reusedCompletedSeed,
                           warmupSeedSeq, warmupSeedAgeMs);
        }
        constexpr const wchar_t* phaseNames[] = {
            L"replay", L"replay_settle", L"trigger", L"tune", L"loop"};
        constexpr const wchar_t* operationNames[] = {
            L"exposure_range", L"exposure_get", L"exposure_set", L"gain_range",
            L"gain_get", L"gain_set", L"exposure_mode", L"exposure_auto",
            L"exposure_state"};
        for (size_t p = 0; p < calls.size(); ++p) {
            for (size_t op = 0; op < calls[p].size(); ++op) {
                const auto& c = calls[p][op];
                if (c.count == 0) continue;
                FACELOGIN_INFO(L"Exposure sensor timing: phase=%s, op=%s, "
                               L"calls=%u, failed=%u, total=%.2f ms, max=%.2f ms",
                               phaseNames[p], operationNames[op], c.count,
                               c.failed, c.totalMs, c.maxMs);
            }
        }
    }
};

inline double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

// Diagnostic snapshots are read on the camera's COM thread, never used for tuning.
class ExposureSnapshot {
public:
    void ReadOnce(const SensorKnobs& sensorKnobs,
                  const std::function<bool(long&, bool&)>& readExposureState,
                  ExposureTimingProfile& profile) {
        if (readDone) return;
        profile.phase = ExposureTimingProfile::Trigger;
        const auto queryAt = std::chrono::steady_clock::now();
        if (readExposureState) {
            const auto exposureAt = std::chrono::steady_clock::now();
            const bool ok = readExposureState(
                exposure, manual);
            auto& c = profile.calls[ExposureTimingProfile::Trigger]
                                          [ExposureTimingProfile::ExposureState];
            const double ms = ElapsedMs(exposureAt);
            ++c.count;
            if (!ok) ++c.failed;
            c.totalMs += ms;
            c.maxMs = std::max(c.maxMs, ms);
            haveExposure = haveMode = ok;
        } else {
            haveExposure = sensorKnobs.exposureGet &&
                sensorKnobs.exposureGet(exposure);
        }
        haveGain = sensorKnobs.gainGet &&
            sensorKnobs.gainGet(gain);
        if (!readExposureState) {
            haveMode = sensorKnobs.exposureIsManual &&
                sensorKnobs.exposureIsManual(manual);
        }
        profile.triggerQueryMs = ElapsedMs(queryAt);
        readDone = true;
    }

    std::wstring Text() const {
        wchar_t knobsText[96] = L"knobs n/a";
        if (haveExposure || haveGain) {
            wchar_t expPart[48] = L"exposure n/a";
            wchar_t gainPart[40] = L"gain n/a";
            if (haveExposure) {
                _snwprintf_s(expPart, _TRUNCATE, L"exposure %ld (%s)", exposure,
                             haveMode ? (manual ? L"manual" : L"auto") : L"mode n/a");
            }
            if (haveGain) _snwprintf_s(gainPart, _TRUNCATE, L"gain %ld", gain);
            _snwprintf_s(knobsText, _TRUNCATE, L"%s, %s", expPart, gainPart);
        }
        return knobsText;
    }

private:
    bool readDone = false;
    long exposure = 0, gain = 0;
    bool manual = false;
    bool haveExposure = false, haveGain = false, haveMode = false;
};

inline SensorKnobs ProfileSensorKnobs(const SensorKnobs& original,
                              ExposureTimingProfile& profile) {
    const auto wrap = [&profile](auto callback, ExposureTimingProfile::Operation op) {
        using Callback = decltype(callback);
        if (!callback) return Callback{};
        return Callback([callback = std::move(callback), &profile, op](auto&&... args) {
            const auto start = std::chrono::steady_clock::now();
            const bool ok = callback(std::forward<decltype(args)>(args)...);
            auto& c = profile.calls[profile.phase][op];
            const double ms = ElapsedMs(start);
            ++c.count;
            if (!ok) ++c.failed;
            c.totalMs += ms;
            c.maxMs = std::max(c.maxMs, ms);
            return ok;
        });
    };
    SensorKnobs result = original;
    result.exposureRange = wrap(original.exposureRange, ExposureTimingProfile::ExposureRange);
    result.exposureGet = wrap(original.exposureGet, ExposureTimingProfile::ExposureGet);
    result.exposureSet = wrap(original.exposureSet, ExposureTimingProfile::ExposureSet);
    result.gainRange = wrap(original.gainRange, ExposureTimingProfile::GainRange);
    result.gainGet = wrap(original.gainGet, ExposureTimingProfile::GainGet);
    result.gainSet = wrap(original.gainSet, ExposureTimingProfile::GainSet);
    result.exposureIsManual = wrap(original.exposureIsManual, ExposureTimingProfile::ExposureMode);
    result.exposureSetAuto = wrap(original.exposureSetAuto, ExposureTimingProfile::ExposureAuto);
    return result;
}

// Per-round binding diagnostics; RAII logs cancellation and rejection too.
struct NormLog {
    std::vector<float> norms;
    unsigned long long attempts = 0, repeatedFrames = 0;
    unsigned long long firstSequence = 0, lastSequence = 0;
    void add(float n, unsigned long long sequence) {
        if (attempts == 0) firstSequence = sequence;
        else if (sequence == lastSequence) ++repeatedFrames;
        lastSequence = sequence;
        ++attempts;
        if (norms.size() < 16) norms.push_back(n);
    }
    ~NormLog() {
        if (norms.empty()) return;
        wchar_t buf[16 * 8];
        size_t off = 0;
        for (size_t i = 0; i < norms.size() && off + 8 < ARRAYSIZE(buf); i++) {
            off += static_cast<size_t>(swprintf(buf + off, 8, L"%.3f,", norms[i]));
        }
        if (off > 0 && buf[off - 1] == L',') buf[--off] = L'\0';
        FACELOGIN_INFO(L"Embedding norms pre-normalize (binding attempts): [%s]",
                       buf);
        FACELOGIN_INFO(L"Binding capture sequence: attempts=%llu, repeated=%llu, "
                       L"first=%llu, last=%llu",
                       attempts, repeatedFrames, firstSequence, lastSequence);
    }
};

} // namespace facelogin::auth_diagnostics
