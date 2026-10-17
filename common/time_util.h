#pragma once

// Single implementation of the local calendar-day stamp used by template
// learning (daily accepted-counters and the alpha decay reset) and the
// service's daily users.dat.bak refresh.

#include <windows.h>

namespace facelogin {

// Local calendar day as a comparable yyyymmdd int; the value steps at
// local midnight.
inline int LocalDayStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return static_cast<int>(st.wYear) * 10000 + st.wMonth * 100 + st.wDay;
}

} // namespace facelogin
