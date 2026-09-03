#pragma once

#include <windows.h>
#include <timeapi.h>

#include "../common/logger.h"

#pragma comment(lib, "winmm.lib")

namespace facelogin {

// Windows 默认时钟节拍 ~15.6 ms:未提精度的进程里 PeekNamedPipe+Sleep 轮询的
// Sleep(1) 实睡一整拍。2026-09-03 双机埋点实测,父服务拾取 worker 消息平均
// 每条等半拍、终态固定挨一拍,认证关键路径合计开发机 ~40 ms / 慢机 ~20 ms;
// worker 进程因加载了 DirectShow/ORT 意外获得 ~1ms 粒度,常驻父服务没有。
// 认证窗口内把定时器精度提到 1ms 收回这部分等待。Win11 起 timeBeginPeriod
// 只作用于调用进程,不影响全系统功耗,锁屏空闲期预载的 worker 同样安全。
class ScopedTimerResolution {
public:
    explicit ScopedTimerResolution(UINT periodMs)
        : m_periodMs(periodMs), m_raised(timeBeginPeriod(periodMs) == TIMERR_NOERROR) {
        if (!m_raised) {
            FACELOGIN_WARN(L"timeBeginPeriod(%u) failed — pipe pickups keep the "
                           L"default ~15.6 ms tick", periodMs);
        }
    }

    ~ScopedTimerResolution() {
        if (m_raised) timeEndPeriod(m_periodMs);
    }

    ScopedTimerResolution(const ScopedTimerResolution&) = delete;
    ScopedTimerResolution& operator=(const ScopedTimerResolution&) = delete;

private:
    UINT m_periodMs;
    bool m_raised;
};

} // namespace facelogin
