#include "performance_affinity.h"

#include "../common/logger.h"

#include <algorithm>
#include <vector>

namespace facelogin {

DWORD_PTR GetPerformanceCoreMask() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) return 0;

    std::vector<BYTE> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &length)) {
        return 0;
    }

    BYTE maxEfficiencyClass = 0;
    for (size_t offset = 0; offset < buffer.size();) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            buffer.data() + offset);
        if (info->Size == 0 || offset + info->Size > buffer.size()) return 0;
        if (info->Relationship == RelationProcessorCore) {
            maxEfficiencyClass = (std::max)(maxEfficiencyClass,
                                            info->Processor.EfficiencyClass);
        }
        offset += info->Size;
    }
    if (maxEfficiencyClass == 0) return 0;

    DWORD_PTR mask = 0;
    for (size_t offset = 0; offset < buffer.size();) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            buffer.data() + offset);
        if (info->Size == 0 || offset + info->Size > buffer.size()) return 0;
        if (info->Relationship == RelationProcessorCore &&
            info->Processor.EfficiencyClass == maxEfficiencyClass) {
            // FaceLogin is x64 and currently restricted to a single processor
            // group. Do not silently form an invalid multi-group affinity mask.
            if (info->Processor.GroupCount != 1) return 0;
            mask |= info->Processor.GroupMask[0].Mask;
        }
        offset += info->Size;
    }
    return mask;
}

ScopedPerformanceCoreAffinity::ScopedPerformanceCoreAffinity(DWORD_PTR mask)
    : m_active(mask != 0) {
    if (!m_active) return;

    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &m_oldMask, &systemMask) ||
        !SetProcessAffinityMask(GetCurrentProcess(), mask)) {
        FACELOGIN_WARN(L"SetProcessAffinityMask failed: %lu", GetLastError());
        m_active = false;
        return;
    }
    FACELOGIN_INFO(L"P-core affinity enabled (mask 0x%llX)",
                   static_cast<unsigned long long>(mask));
}

ScopedPerformanceCoreAffinity::~ScopedPerformanceCoreAffinity() {
    if (m_active) SetProcessAffinityMask(GetCurrentProcess(), m_oldMask);
}

} // namespace facelogin
