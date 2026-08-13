#pragma once

#include <windows.h>

namespace facelogin {

// Returns a mask containing the performance cores on a hybrid CPU, or 0 when
// the processor is uniform / cannot be queried. The implementation supports
// the process-affinity APIs used by the current x64 deployment target.
DWORD_PTR GetPerformanceCoreMask();

// Pins the current process only for the lifetime of this object, restoring the
// old affinity mask on every exit path. Authentication lives in a one-shot
// worker, so its scope never changes the parent service's scheduling policy.
class ScopedPerformanceCoreAffinity {
public:
    explicit ScopedPerformanceCoreAffinity(DWORD_PTR mask);
    ~ScopedPerformanceCoreAffinity();

    ScopedPerformanceCoreAffinity(const ScopedPerformanceCoreAffinity&) = delete;
    ScopedPerformanceCoreAffinity& operator=(const ScopedPerformanceCoreAffinity&) = delete;

private:
    bool m_active = false;
    DWORD_PTR m_oldMask = 0;
};

} // namespace facelogin
