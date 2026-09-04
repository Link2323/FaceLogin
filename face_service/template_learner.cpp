#include "template_learner.h"

#include "../common/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace facelogin {

namespace {

float L2Norm(const std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    return static_cast<float>(std::sqrt(sum));
}

// Local calendar day as yyyymmdd — the α decay counter resets at midnight.
int LocalDayStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return static_cast<int>(st.wYear) * 10000 + st.wMonth * 100 + st.wDay;
}

} // namespace

TemplateLearner::TemplateLearner(LearningConfig config, Hooks hooks)
    : m_config(config), m_hooks(std::move(hooks)) {
    InitializeCriticalSection(&m_cs);
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wake) {
        FACELOGIN_ERROR(L"TemplateLearner: CreateEvent failed (%lu)", GetLastError());
        return;
    }
    m_thread = std::thread(&TemplateLearner::Run, this);
    FACELOGIN_INFO(L"Template learner started: gate=%.2f norm_floor=%.1f alpha=%.2f interval=%ds%s",
                   m_config.distanceGate, m_config.normFloor, m_config.alpha,
                   m_config.minIntervalSec, m_config.enabled ? L"" : L" (disabled)");
}

TemplateLearner::~TemplateLearner() {
    Stop();
    if (m_wake) {
        CloseHandle(m_wake);
        m_wake = nullptr;
    }
    DeleteCriticalSection(&m_cs);
}

void TemplateLearner::Enqueue(LearningSample sample) {
    if (m_stopped.load(std::memory_order_acquire)) return;
    {
        EnterCriticalSection(&m_cs);
        if (!m_pending) {
            m_pending.emplace(std::move(sample));
            m_pendingSince = std::chrono::steady_clock::now();
        } else if (sample.distance < m_pending->distance) {
            // Same burst: keep only the round's best frame (min distance).
            m_pending = std::move(sample);
        }
        // else: keep the already-pending (better) sample.
        LeaveCriticalSection(&m_cs);
    }
    if (m_wake) SetEvent(m_wake);
}

void TemplateLearner::UpdateConfig(LearningConfig config) {
    EnterCriticalSection(&m_cs);
    m_config = config;
    LeaveCriticalSection(&m_cs);
    FACELOGIN_INFO(L"Template learner config updated: gate=%.2f norm_floor=%.1f alpha=%.2f interval=%ds%s",
                   config.distanceGate, config.normFloor, config.alpha,
                   config.minIntervalSec, config.enabled ? L"" : L" (disabled)");
}

LearningConfig TemplateLearner::Config() const {
    EnterCriticalSection(&m_cs);
    LearningConfig copy = m_config;
    LeaveCriticalSection(&m_cs);
    return copy;
}

void TemplateLearner::Stop() {
    if (m_stopped.exchange(true, std::memory_order_acq_rel)) return;
    if (m_wake) SetEvent(m_wake);
    if (m_thread.joinable()) m_thread.join();
    bool dirty = false;
    EnterCriticalSection(&m_cs);
    dirty = m_dirty;
    m_dirty = false;
    LeaveCriticalSection(&m_cs);
    if (dirty) {
        m_hooks.flush();
        FACELOGIN_INFO(L"Template learner: pending updates flushed on stop");
    }
}

std::vector<float> TemplateLearner::BlendEMA(const std::vector<float>& tmpl,
                                             const std::vector<float>& sample,
                                             float alpha) {
    if (tmpl.size() != sample.size() || tmpl.empty()) return {};
    std::vector<float> out(tmpl.size());
    double sum = 0.0;
    for (size_t i = 0; i < tmpl.size(); i++) {
        out[i] = alpha * sample[i] + (1.0f - alpha) * tmpl[i];
        sum += static_cast<double>(out[i]) * out[i];
    }
    const double norm = std::sqrt(sum);
    if (norm > 1e-12) {
        for (float& v : out) v = static_cast<float>(v / norm);
    }
    return out;
}

void TemplateLearner::ApplySample(const LearningSample& sample) {
    const LearningConfig cfg = Config();
    if (!cfg.enabled) {
        FACELOGIN_INFO(L"Template update skipped: learning disabled");
        return;
    }
    // Red line 1 (2026-09-03 revision): the effective gate follows the
    // user's own era distribution, capped by the configured ceiling.
    const float gate = (sample.userEraP20 > 0.0f)
        ? std::min(sample.userEraP20, cfg.distanceGate)
        : cfg.distanceGate;
    if (sample.distance > gate) {
        FACELOGIN_INFO(L"Template update skipped: distance %.3f > gate %.3f "
                       L"(user era p20 %.3f, cap %.2f)",
                       sample.distance, gate, sample.userEraP20,
                       cfg.distanceGate);
        return;
    }
    if (sample.norm < cfg.normFloor) {
        FACELOGIN_INFO(L"Template update skipped: norm %.2f < floor %.2f",
                       sample.norm, cfg.normFloor);
        return;
    }
    const auto now = std::chrono::system_clock::now();
    {
        EnterCriticalSection(&m_cs);
        const auto sinceAccept = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_lastAccept).count();
        if (m_lastAccept.time_since_epoch().count() != 0 &&
            sinceAccept < cfg.minIntervalSec) {
            LeaveCriticalSection(&m_cs);
            FACELOGIN_INFO(L"Template update skipped: interval %llds < %ds",
                           static_cast<long long>(sinceAccept), cfg.minIntervalSec);
            return;
        }
        LeaveCriticalSection(&m_cs);
    }

    // Fetch, blend, commit — the hooks serialize CredentialStore access.
    std::vector<float> tmpl;
    if (!m_hooks.fetchTemplate(sample.sid, sample.faceId, tmpl) || tmpl.empty()) {
        FACELOGIN_INFO(L"Template update skipped: face #%u disappeared",
                       sample.faceId);
        return;
    }
    const int day = LocalDayStamp();
    int decayed = 1;
    {
        EnterCriticalSection(&m_cs);
        if (day != m_dayStamp) {
            m_dayStamp = day;
            m_dayCount = 0;
        }
        decayed = m_dayCount + 1;
        LeaveCriticalSection(&m_cs);
    }
    // Drift guard: the more updates accepted today, the smaller each step.
    const float alphaUsed = cfg.alpha / static_cast<float>(decayed);
    std::vector<float> blended = BlendEMA(tmpl, sample.embedding, alphaUsed);
    if (blended.empty()) return;

    const float oldNorm = L2Norm(tmpl);
    const float newNorm = L2Norm(blended);
    if (!m_hooks.commit(sample, blended, alphaUsed)) {
        // Rejected by the store (sentinel) or the target vanished — logged
        // upstream. Deliberately no retry: the next auth round re-derives.
        return;
    }
    {
        EnterCriticalSection(&m_cs);
        m_dayCount = decayed;
        m_lastAccept = now;
        m_dirty = true;
        m_lastUpdate = now;
        m_generations.push_back({sample.sid, sample.faceId, std::move(tmpl)});
        while (m_generations.size() > 3) m_generations.pop_front();
        const size_t gen = m_generations.size();
        LeaveCriticalSection(&m_cs);
        FACELOGIN_INFO(L"Template updated: user=%s face#%u(%s) probe_dist=%.3f "
                       L"alpha=%.3f probe_norm=%.2f tpl_norm %.4f->%.4f gen=%zu",
                       sample.username.c_str(), sample.faceId,
                       sample.faceLabel.c_str(), sample.distance, alphaUsed,
                       sample.norm, oldNorm, newNorm, gen);
    }
}

void TemplateLearner::Run() {
    while (!m_stopped.load(std::memory_order_acquire)) {
        WaitForSingleObject(m_wake, 200);

        std::optional<LearningSample> toProcess;
        bool flushDue = false;
        const auto steadyNow = std::chrono::steady_clock::now();
        const auto sysNow = std::chrono::system_clock::now();
        {
            EnterCriticalSection(&m_cs);
            if (m_pending &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    steadyNow - m_pendingSince).count() >= kStartDelayMs) {
                toProcess = std::move(m_pending);
                m_pending.reset();
            }
            if (m_dirty &&
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    sysNow - m_lastUpdate).count() >= kFlushQuietMs) {
                flushDue = true;
            }
            LeaveCriticalSection(&m_cs);
        }

        if (toProcess) ApplySample(*toProcess);
        if (flushDue) {
            bool dirty = false;
            EnterCriticalSection(&m_cs);
            dirty = m_dirty;
            m_dirty = false;
            LeaveCriticalSection(&m_cs);
            if (dirty) {
                m_hooks.flush();
                FACELOGIN_INFO(L"Template learner: users.dat flushed (debounce)");
            }
        }
    }
}

} // namespace facelogin
