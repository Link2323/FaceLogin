#include "FaceLoginCredential.h"
#include "FaceLoginProvider.h"
#include "resource.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/secure_clear.h"
#include <wincred.h>
#include <ntstatus.h>
#include <ntsecapi.h>
#include <shlwapi.h>
#include <process.h>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "ntdll.lib")

// ============================================================================
// Input-detection thread (LOGON + unlock scenarios)
// ============================================================================
//
// Runs as a background thread, polling GetLastInputInfo() every 50 ms.
// When it detects that the user has pressed a key or moved the mouse AFTER
// the baseline tick (recorded in Advise()), it calls StartAuth() which
// connects the pipe asynchronously.  Once auth completes, the pipe callback
// stores credentials and triggers CredentialsChanged(), causing LogonUI to
// re-enumerate and call GetSerialization(), which then packs and returns
// the ready credentials.
//
// Trigger algorithm (residue quarantine + quiesce):
//
//   Lock-shortcut residue. Lock hotkeys fire MID-GESTURE (Win+L activates on
//   the L keydown), and holding Win+L past the moment the credential view
//   appears leaves the trailing auto-repeat/KEYUP ticks landing AFTER the
//   baseline. GetLastInputInfo() alone cannot tell those ticks from a
//   genuine dismiss press, so the wave would auto-trigger auth and unlock
//   the user right back (reproduced 2026-08-15: hold Win+L -> self-unlock
//   ~2s after lock). Advise() therefore snapshots, via GetAsyncKeyState(),
//   EVERY key still physically held at baseline — any held key at that
//   moment means the lock action is still mid-gesture. Note it cannot be a
//   modifier-only fingerprint: Windows clears the Win modifier's async key
//   state at secure-desktop activation (observed 2026-08-15: snapshot saw
//   'L' but NOT LWIN), while a held non-modifier keeps refreshing its state
//   via auto-repeat and stays visible. When any key was held, this thread
//   first quarantines the residue: polls until every one is released (capped
//   at RESIDUE_QUARANTINE_CAP_MS so a stuck key state cannot disable
//   auto-trigger forever), waits RESIDUE_MARGIN_MS for the final KEYUP tick,
//   then re-seeds lastInputTick past the whole burst so it can never form a
//   wave. Mouse/touch locking is release-triggered (the gesture completes
//   before the lock engages), so it has no structural residue and needs no
//   quarantine.
//
//   Wave + quiesce. A new input tick starts a wave; once no further tick
//   arrives for QUIESCE_MS AND no key is currently physically held (checked
//   via GetAsyncKeyState — a held key keeps generating auto-repeat ticks, so
//   it must never count as a finished wave), the wave has ended and
//   StartAuth() fires IMMEDIATELY — the user's single dismiss press is the
//   trigger (trade-off: there is no way to "just dismiss" without starting
//   face recognition). QUIESCE_MS only needs to clear the keyboard
//   auto-repeat interval (~33–76 ms observed) and event jitter; the
//   typing-gap floor that kept it at 200 ms was dropped 2026-08-15: the
//   waiting view is face-first with no focused password field, typing a
//   password requires a deliberate tile switch first, and the interaction
//   policy already guarantees auth starting mid-typing can't interrupt the
//   input.
//
//   State per iteration:
//     lastInputTick     — highest input timestamp seen so far (>= baseline)
//     lastInputEndWall  — wall-clock tick of when we last saw a new input
//     armed             — true once the dismiss wave has gone quiet for
//                          QUIESCE_MS; on becoming true we fire StartAuth()
//                          right away and exit the loop.
//
// The thread stops when:
//   - The dismiss wave goes quiet and StartAuth() is auto-triggered, OR
//   - The stop event is signaled (UnAdvise / destructor), OR
//   - UnAdvise() sets m_pCredentialEvents = nullptr and the thread notices

struct InputDetectionContext {
    FaceLoginCredential* pCred;
};

// Keys still held at the baseline moment are the mid-gesture fingerprint of
// a lock action (see the algorithm comment above). Any key counts — see the
// Win-cleared observation there for why modifiers alone are not enough.
static const int kSnapshotFirstVk = 0x01;   // VK_LBUTTON
static const int kSnapshotLastVk = 0xFE;    // VK_OEM_CLEAR

void FaceLoginCredential::SnapshotBaselineKeys() {
    std::wstring held;
    auto appendName = [&held](int vk) {
        wchar_t name[8];
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
            name[0] = static_cast<wchar_t>(vk);
            name[1] = L'\0';
        } else {
            wsprintfW(name, L"#%02X", vk);
        }
        if (!held.empty()) held += L",";
        held += name;
    };

    m_baselineKeysHeld.clear();
    for (int vk = kSnapshotFirstVk; vk <= kSnapshotLastVk; ++vk) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            m_baselineKeysHeld.push_back(vk);
            appendName(vk);
        }
    }

    // One log line proving GetAsyncKeyState works in the secure desktop and
    // showing exactly what was held at baseline.
    FACELOGIN_INFO(L"Advise: baseline held keys: %s — residue quarantine %s",
                   held.empty() ? L"(none)" : held.c_str(),
                   m_baselineKeysHeld.empty() ? L"not needed" : L"ARMED");
}

// True while any key or mouse button is physically held. Used at arm time so
// a held (auto-repeating) key can never end a wave — its eventual KEYUP
// restarts the quiet window instead, regardless of the repeat interval.
static bool AnyKeyPhysicallyDown() {
    for (int vk = kSnapshotFirstVk; vk <= kSnapshotLastVk; ++vk) {
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    }
    return false;
}

static unsigned __stdcall InputDetectionThreadProc(void* pParam) {
    auto* ctx = static_cast<InputDetectionContext*>(pParam);
    FaceLoginCredential* pCred = ctx->pCred;
    delete ctx;

    FACELOGIN_INFO(L"[InputThread] Started — polling for user input every 50ms");

    const DWORD pollIntervalMs = 25;
    const DWORD QUIESCE_MS = 100;         // gap that ends an input "wave" (held keys are excluded by the arm-time guard below)
    const DWORD RESIDUE_MARGIN_MS = 150;  // swallow the final KEYUP ticks after residue keys release
    const DWORD RESIDUE_QUARANTINE_CAP_MS = 5000;  // stuck key state must not disable auto-trigger
    const DWORD timeoutSec = 30;
    DWORD startTick = GetTickCount();

    // Adaptive-quiescence state.  lastInputTick is seeded with the baseline
    // so any input that occurred BEFORE our DLL loaded (i.e. the dismiss
    // KEYDOWN) is ignored and never counts as a new wave.
    DWORD baseline = pCred->m_waitingStartTick;
    DWORD lastInputTick = baseline;     // input-timestamp space (GetLastInputInfo)
    DWORD lastInputEndWall = 0;         // wall-clock space (GetTickCount)
    bool armed = false;                 // current wave has gone quiet

    // Residue quarantine. Keys were still held when this credential appeared
    // (see the algorithm comment above): without this, their trailing
    // auto-repeat/KEYUP ticks would read as a dismiss wave and auto-unlock
    // the user right after they locked. Wait until every one is physically
    // released, let the final KEYUP land inside the margin, then raise the
    // seen-input watermark past the whole burst. The 30s idle restart below
    // never re-quarantines — baseline residue only exists at thread start.
    if (!pCred->m_baselineKeysHeld.empty()) {
        const DWORD quarantineStart = GetTickCount();
        for (;;) {
            if (WaitForSingleObject(pCred->m_hInputStop, 0) == WAIT_OBJECT_0) {
                FACELOGIN_INFO(L"[InputThread] Stop event signaled — exiting (residue quarantine)");
                FACELOGIN_INFO(L"[InputThread] Exiting");
                pCred->m_inputThreadRunning = false;
                return 0;
            }
            if (GetTickCount() - quarantineStart >= RESIDUE_QUARANTINE_CAP_MS) {
                FACELOGIN_WARN(L"[InputThread] Residue keys not released within %lums — capping quarantine",
                               RESIDUE_QUARANTINE_CAP_MS);
                break;
            }
            bool allReleased = true;
            for (int vk : pCred->m_baselineKeysHeld) {
                if (GetAsyncKeyState(vk) & 0x8000) { allReleased = false; break; }
            }
            if (allReleased) break;
            SleepEx(pollIntervalMs, TRUE);
        }
        SleepEx(RESIDUE_MARGIN_MS, TRUE);
        LASTINPUTINFO residueLii = {};
        residueLii.cbSize = sizeof(residueLii);
        if (GetLastInputInfo(&residueLii) && residueLii.dwTime > lastInputTick) {
            lastInputTick = residueLii.dwTime;
        }
        FACELOGIN_INFO(L"[InputThread] Residue quarantine ended after %lums — residue ticks swallowed",
                       GetTickCount() - quarantineStart);
    }

    // Loop forever (until the stop event is signaled).  The 30s timeout does
    // NOT kill the thread — it only restarts the idle window so a user who
    // waits longer than 30s before pressing a key can still trigger auth.
    while (true) {
        // Check stop signal (non-blocking)
        DWORD waitResult = WaitForSingleObject(pCred->m_hInputStop, 0);
        if (waitResult == WAIT_OBJECT_0) {
            FACELOGIN_INFO(L"[InputThread] Stop event signaled — exiting");
            break;
        }

        // Idle-window timeout: restart the window instead of exiting, so a
        // late keypress still works. (Previously the thread exited after 30s
        // of no input, leaving no path to restart it — a later keypress did
        // nothing.)  Also reset the adaptive state so the next wave is
        // evaluated cleanly.
        DWORD elapsedMs = GetTickCount() - startTick;
        if (elapsedMs > timeoutSec * 1000) {
            FACELOGIN_INFO(L"[InputThread] 30s idle — restarting idle window");
            startTick = GetTickCount();
            baseline = pCred->m_waitingStartTick;
            lastInputTick = baseline;
            lastInputEndWall = 0;
            armed = false;
            continue;
        }

        // Poll GetLastInputInfo
        LASTINPUTINFO lii = {};
        lii.cbSize = sizeof(lii);
        if (GetLastInputInfo(&lii)) {
            bool newInput = (lii.dwTime > lastInputTick);

            if (newInput) {
                // Part of (or the start of) the dismiss wave (KEYDOWN/KEYUP).
                // Note the wall-clock moment we saw it so we can detect when
                // the wave goes quiet, then auto-trigger.
                lastInputTick = lii.dwTime;
                lastInputEndWall = GetTickCount();
                armed = false;
            }

            // Arm once the current wave has been quiet for QUIESCE_MS AND no
            // key is physically held (a held key is still generating its
            // wave). lastInputEndWall==0 means we've never seen a
            // post-baseline input yet, so there's nothing to arm against.
            if (!armed && lastInputEndWall != 0 &&
                GetTickCount() - lastInputEndWall >= QUIESCE_MS &&
                !AnyKeyPhysicallyDown()) {
                armed = true;
                FACELOGIN_INFO(L"[InputThread] Dismiss wave quiet for %lums — "
                              L"auto-triggering (no second keypress required)",
                              QUIESCE_MS);
                // Auto-trigger: the dismiss wave (KEYDOWN+KEYUP) has fully
                // ended, so the user's single dismiss press is enough.
                // Start auth immediately instead of waiting for another wave.
                FACELOGIN_INFO(L"[InputThread] Auto-triggered after dismiss "
                              L"(last=%lu, baseline=%lu, elapsed=%lums)",
                              lastInputTick, baseline,
                              static_cast<DWORD>(lastInputTick - baseline));
                pCred->StartAuth();
                break;
            }
        }

        // Sleep (alertable so the stop event can wake us). While a wave is
        // being timed out, wake exactly when the quiet window can complete
        // instead of drifting up to a full poll interval past it.
        DWORD sleepMs = pollIntervalMs;
        if (lastInputEndWall != 0) {
            const DWORD quietSoFar = GetTickCount() - lastInputEndWall;
            if (quietSoFar < QUIESCE_MS && QUIESCE_MS - quietSoFar < sleepMs) {
                sleepMs = QUIESCE_MS - quietSoFar + 1;
            }
        }
        SleepEx(sleepMs, TRUE);
    }

    FACELOGIN_INFO(L"[InputThread] Exiting");
    pCred->m_inputThreadRunning = false;
    return 0;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

FaceLoginCredential::FaceLoginCredential() {
    InitializeCriticalSection(&m_cs);
    m_csInitialized = true;

    m_hCredsReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_hCredsReady) {
        FACELOGIN_ERROR(L"Failed to create credentials ready event");
    }

    FACELOGIN_DEBUG(L"FaceLoginCredential created");
}

FaceLoginCredential::~FaceLoginCredential() {
    // SENSITIVE: Zero the password from memory
    facelogin::SecureClearWString(m_password);

    // Stop the background input-detection thread before tearing down any state
    // it touches. COM release order does not guarantee UnAdvise (which also
    // calls StopInputDetectionThread) runs before the destructor — if the
    // thread is still running when `this` is freed, its next access to pCred
    // is a use-after-free. Idempotent and safe to call when not running.
    StopInputDetectionThread();

    if (m_hCredsReady) {
        CloseHandle(m_hCredsReady);
        m_hCredsReady = nullptr;
    }

    if (m_csInitialized) {
        DeleteCriticalSection(&m_cs);
        m_csInitialized = false;
    }

    FACELOGIN_DEBUG(L"FaceLoginCredential destroyed");
}

void FaceLoginCredential::Initialize(FaceLoginProvider* pProvider) {
    m_pProvider = pProvider;
    FACELOGIN_DEBUG(L"FaceLoginCredential initialized with provider");
}

void FaceLoginCredential::AdviseProvider(ICredentialProviderEvents* pEvents, UINT_PTR upAdviseContext) {
    m_pProviderEvents = pEvents;
    m_upAdviseContext = upAdviseContext;
}

void FaceLoginCredential::UnadviseProvider() {
    m_pProviderEvents = nullptr;
    m_upAdviseContext = 0;
}

// ============================================================================
// IUnknown
// ============================================================================

STDMETHODIMP FaceLoginCredential::QueryInterface(REFIID riid, void** ppv) {
    *ppv = nullptr;

    if (riid == IID_IUnknown ||
        riid == IID_ICredentialProviderCredential) {
        *ppv = static_cast<ICredentialProviderCredential*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) FaceLoginCredential::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) FaceLoginCredential::Release() {
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// ICredentialProviderCredential — Advise/UnAdvise
// ============================================================================

STDMETHODIMP FaceLoginCredential::Advise(ICredentialProviderCredentialEvents* pcpce) {
    FACELOGIN_INFO(L"=== Advise ENTER (state=%d, pcpce=%p) ===", static_cast<int>(m_state), pcpce);

    if (m_pCredentialEvents) {
        m_pCredentialEvents->Release();
    }
    m_pCredentialEvents = pcpce;
    if (m_pCredentialEvents) {
        m_pCredentialEvents->AddRef();
    }

    // Guard: if we already have credentials ready from a previous
    // auth round, don't restart the flow.  This prevents an infinite
    // loop where OnPipeResponse → CredentialsChanged → Advise()
    // overwrites Ready back to Authenticating.
    if (m_state == State::Ready && !m_password.empty()) {
        FACELOGIN_INFO(L"Advise: credentials already ready, skipping auth restart");
        return S_OK;
    }

    // A terminal failure must remain inert until the user explicitly clicks
    // the retry command. LogonUI may call Advise again for reasons unrelated
    // to FaceLogin; restarting GetLastInputInfo polling here would interpret
    // password-entry keystrokes as face retries and repeatedly steal focus.
    if (facelogin::credential_provider::IsRetryableFailure(m_state)) {
        FACELOGIN_INFO(L"Advise: terminal failure requires explicit retry; "
                       L"input detection remains disabled");
        return S_OK;
    }

    // Guard: if we're already authenticating and have a live pipe,
    // don't create a second connection.
    if (m_state == State::Authenticating && m_pipeClient && m_pipeClient->IsConnected()) {
        FACELOGIN_INFO(L"Advise: already authenticating, skipping auth restart");
        return S_OK;
    }

    // Start a background thread that polls GetLastInputInfo() for new
    // keyboard/mouse input (the same flow for LOGON and UNLOCK — the
    // provider no longer auto-triggers on cold boot, which started
    // recognizing before the user was at the machine and, because every
    // failed attempt re-enumerated with coldBoot still true, looped
    // recognition forever on an empty scene).  When input is detected,
    // the thread calls StartAuth() which connects the pipe
    // asynchronously. The pipe callback stores credentials and triggers
    // CredentialsChanged(), causing LogonUI to re-enumerate and call
    // GetSerialization() to retrieve ready credentials.
    bool credUI = m_pProvider ? m_pProvider->IsCredUI() : false;
    FACELOGIN_INFO(L"Advise: credUI=%d, m_pProvider=%p", credUI, m_pProvider);
    if (!facelogin::credential_provider::ShouldStartInputDetection(m_state)) {
        FACELOGIN_INFO(L"Advise: state does not permit passive input detection");
        return S_OK;
    }
    m_waitingStartTick = GetTickCount();
    FACELOGIN_INFO(L"Advise: baseline tick = %lu", m_waitingStartTick);
    // Same instant as the baseline tick: capture which lock-shortcut
    // modifiers are still physically held, before the input thread starts.
    SnapshotBaselineKeys();
    StartInputDetectionThread();

    FACELOGIN_INFO(L"=== Advise EXIT (state=%d) ===", static_cast<int>(m_state));
    return S_OK;
}

STDMETHODIMP FaceLoginCredential::UnAdvise() {
    FACELOGIN_INFO(L"=== UnAdvise ENTER ===");

    // Stop the input-detection thread if running
    StopInputDetectionThread();

    if (m_pCredentialEvents) {
        m_pCredentialEvents->Release();
        m_pCredentialEvents = nullptr;
    }

    m_pipeClient.reset();
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — SetSelected/SetDeselected
// ============================================================================

STDMETHODIMP FaceLoginCredential::SetSelected(BOOL* pbAutoLogon) {
    FACELOGIN_INFO(L"=== SetSelected ENTER (state=%d, *pbAutoLogon=%d) ===",
                  static_cast<int>(m_state),
                  pbAutoLogon ? static_cast<int>(*pbAutoLogon) : -1);

    bool credUI = m_pProvider ? m_pProvider->IsCredUI() : false;

    if (m_state == State::Ready) {
        // Credentials ready (bg thread finished auth):
        // enable auto-logon so LogonUI calls GetSerialization to pack creds.
        *pbAutoLogon = TRUE;
    } else {
        // Still Waiting: no auto-logon; we wait for the bg thread.
        *pbAutoLogon = FALSE;
    }

    FACELOGIN_INFO(L"=== SetSelected EXIT (*pbAutoLogon=%d, credUI=%d, state=%d) ===",
                  *pbAutoLogon, static_cast<int>(credUI),
                  static_cast<int>(m_state));
    return S_OK;
}

STDMETHODIMP FaceLoginCredential::SetDeselected() {
    FACELOGIN_INFO(L"=== SetDeselected called (state=%d) ===", static_cast<int>(m_state));
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — Field State / Values
// ============================================================================

STDMETHODIMP FaceLoginCredential::GetFieldState(
    DWORD dwFieldID,
    CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) {

    FACELOGIN_INFO(L"=== GetFieldState (field=%lu, state=%d) ===",
                  dwFieldID, static_cast<int>(m_state));

    *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
    *pcpfis = CPFIS_NONE;

    switch (dwFieldID) {
    case 0: // "Face Login" label
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case 1: // Status text — always visible so error states are seen
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case 2: // Submit button — hidden in both scenarios
        *pcpfs = CPFS_HIDDEN;
        break;

    case 3: // Retry is explicit after failure; otherwise retain password link.
        *pcpfs = facelogin::credential_provider::IsRetryableFailure(m_state)
            ? CPFS_DISPLAY_IN_SELECTED_TILE
            : CPFS_DISPLAY_IN_DESELECTED_TILE;
        break;

    default:
        return E_INVALIDARG;
    }

    return S_OK;
}

STDMETHODIMP FaceLoginCredential::GetStringValue(DWORD dwFieldID, PWSTR* ppwsz) {
    FACELOGIN_INFO(L"=== GetStringValue (field=%lu, state=%d) ===",
                  dwFieldID, static_cast<int>(m_state));
    *ppwsz = nullptr;

    switch (dwFieldID) {
    case 0: // Label
        return SHStrDupW(L"人脸登录", ppwsz);

    case 1: // Status
        switch (m_state) {
        case State::Waiting:
            return SHStrDupW(L"按下任意按键以开始人脸识别", ppwsz);
        case State::Authenticating:
            if (!m_statusText.empty()) {
                return SHStrDupW(m_statusText.c_str(), ppwsz);
            }
            return SHStrDupW(L"正在识别...", ppwsz);
        case State::Ready:
            return SHStrDupW(L"人脸识别成功，正在解锁...", ppwsz);
        case State::Failed:
            // Surface a specific failure reason if one was provided by the
            // service (e.g. "未检测到人脸", "未通过活体检测，请使用真实人脸",
            // "识别超时，请重试"); otherwise the generic fallback.
            if (!m_statusText.empty()) {
                return SHStrDupW(m_statusText.c_str(), ppwsz);
            }
            return SHStrDupW(L"人脸识别失败，请重试或使用密码登录", ppwsz);
        case State::Error:
            // Show the specific error message from the service (e.g. anti-spoof
            // rejection) if one was received; otherwise the generic fallback.
            if (!m_statusText.empty()) {
                return SHStrDupW(m_statusText.c_str(), ppwsz);
            }
            return SHStrDupW(L"人脸登录服务不可用", ppwsz);
        default:
            return SHStrDupW(L"", ppwsz);
        }

    case 2: // Submit button
        return SHStrDupW(L"", ppwsz);

    case 3: // Command link
        return SHStrDupW(
            facelogin::credential_provider::IsRetryableFailure(m_state)
                ? L"重新尝试人脸识别"
                : L"切换到密码登录",
            ppwsz);

    default:
        return E_INVALIDARG;
    }
}

STDMETHODIMP FaceLoginCredential::GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp) {
    UNREFERENCED_PARAMETER(dwFieldID);
    *phbmp = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::GetSubmitButtonValue(DWORD dwFieldID, DWORD* pdwAdjacentTo) {
    if (dwFieldID == 2) {
        *pdwAdjacentTo = 1; // Next to the status text field
        return S_OK;
    }
    return E_INVALIDARG;
}

STDMETHODIMP FaceLoginCredential::GetCheckboxValue(DWORD dwFieldID, BOOL* pbChecked, PWSTR* ppwszLabel) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pbChecked);
    UNREFERENCED_PARAMETER(ppwszLabel);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::GetComboBoxValueCount(DWORD dwFieldID, DWORD* pcItems, DWORD* pdwSelectedItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pcItems);
    UNREFERENCED_PARAMETER(pdwSelectedItem);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR* ppwszItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(dwItem);
    UNREFERENCED_PARAMETER(ppwszItem);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::SetStringValue(DWORD dwFieldID, LPCWSTR pwz) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(pwz);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(bChecked);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) {
    UNREFERENCED_PARAMETER(dwFieldID);
    UNREFERENCED_PARAMETER(dwSelectedItem);
    return E_NOTIMPL;
}

STDMETHODIMP FaceLoginCredential::CommandLinkClicked(DWORD dwFieldID) {
    if (dwFieldID == 3) {
        if (facelogin::credential_provider::IsRetryableFailure(m_state)) {
            FACELOGIN_INFO(L"User explicitly requested face authentication retry");
            StartExplicitRetry();
            return S_OK;
        }
        FACELOGIN_INFO(L"User clicked 'Switch to password login'");
        SwitchToPasswordProvider();
        return S_OK;
    }
    return E_INVALIDARG;
}

// ============================================================================
// ICredentialProviderCredential — GetSerialization (THE KEY METHOD)
// ============================================================================

STDMETHODIMP FaceLoginCredential::GetSerialization(
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
    PWSTR* ppwszOptionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) {

    FACELOGIN_INFO(L"=== GetSerialization ENTER (state=%d) ===", static_cast<int>(m_state));

    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    ZeroMemory(pcpcs, sizeof(*pcpcs));

    // Unlock scenario: if we're in Waiting state, the background input-
    // detection thread is still waiting for user input. Return "not
    // finished" — no credentials yet.
    if (m_state == State::Waiting) {
        FACELOGIN_INFO(L"GetSerialization: still Waiting for user input");
        return S_OK;
    }

    // If we're in Error state, service is not available — don't block login
    if (m_state == State::Error) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Auth failed earlier — don't retry, let user use password
    if (m_state == State::Failed) {
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Check for pipe disconnection — the server may have disconnected
    // without sending a response (e.g., service crashed or timed out).
    // Without this check, the CP stays in Authenticating forever.
    if (m_pipeClient && !m_pipeClient->IsConnected()) {
        FACELOGIN_WARN(L"Pipe disconnected while waiting for auth response");
        PresentRetryableFailure(State::Error, L"人脸登录服务连接已断开");
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // Track auth timeout: if we've been authenticating too long, give up
    if (m_state == State::Authenticating) {
        LONGLONG now = 0;
        GetSystemTimeAsFileTime(reinterpret_cast<FILETIME*>(&now));
        if (m_authStartTime == 0) {
            m_authStartTime = now;
        } else {
            // Preserve the established public-auth timeout: 15s service
            // window plus 5s grace for pipe delivery/serialization.
            const LONGLONG AUTH_TIMEOUT_100NS = 200000000LL;
            if (now - m_authStartTime > AUTH_TIMEOUT_100NS) {
                FACELOGIN_WARN(L"Auth timed out waiting for service response");
                PresentRetryableFailure(State::Failed, L"识别超时，请重试");
                *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
                return S_OK;
            }
        }
    }

    // Check if pipe has a response
    if (m_pipeClient) {
        std::wstring response;
        if (m_pipeClient->CheckResponse(response)) {
            // Parse the response
            auto result = facelogin::ipc::ParseAuthMessage(response);

            if (result.status == facelogin::ipc::AuthResult::Status::Success) {
                FACELOGIN_INFO(L"Auth success: %s\\%s (SID=%s, UPN=%s)",
                              result.domain.c_str(), result.username.c_str(),
                              result.sid.c_str(), result.upn.c_str());
                m_upn = result.upn;
                m_domain = result.domain;
                m_username = result.username;
                m_password = result.password;
                m_state = State::Ready;
                m_statusText = L"人脸识别成功，正在解锁...";
                if (m_pCredentialEvents) {
                    m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
                }
                if (m_hCredsReady) {
                    SetEvent(m_hCredsReady);
                }
            }
            else if (result.status == facelogin::ipc::AuthResult::Status::Timeout) {
                FACELOGIN_INFO(L"Auth timeout");
                PresentRetryableFailure(State::Failed, L"识别超时，请重试");
                *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
                return S_OK;
            }
            else if (result.status == facelogin::ipc::AuthResult::Status::Error) {
                FACELOGIN_WARN(L"Auth error: %s", result.errorMessage.c_str());
                // Surface the service's specific error on the lock screen.
                if (!result.errorMessage.empty()) {
                    m_statusText = result.errorMessage;
                }
                PresentRetryableFailure(
                    State::Error,
                    result.errorMessage.empty()
                        ? L"人脸登录服务不可用"
                        : result.errorMessage);
                *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
                return S_OK;
            }
        }
    }

    // If we have credentials ready, pack and return them
    if (m_state == State::Ready && !m_password.empty()) {
        // NOTE: never log the password or any part of it — it is a credential.
        HRESULT hr = PackCredentials(pcpcs);
        if (SUCCEEDED(hr)) {
            *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
            FACELOGIN_INFO(L"PackCred SUCCESS: cbSerialization=%lu, ulAuthPackage=%lu",
                          pcpcs->cbSerialization, pcpcs->ulAuthenticationPackage);
        } else {
            FACELOGIN_ERROR(L"PackCred FAILED: hr=0x%08X", hr);
        }
        return hr;
    }

    // Not ready yet — LogonUI will call GetSerialization() again
    // due to auto-logon being set. Our auth timeout guard above
    // ensures we eventually give up and don't block forever.
    FACELOGIN_INFO(L"=== GetSerialization EXIT: not ready (state=%d, response=%d) ===",
                  static_cast<int>(m_state), static_cast<int>(*pcpgsr));
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — ReportResult
// ============================================================================

STDMETHODIMP FaceLoginCredential::ReportResult(
    NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
    PWSTR* ppwszOptionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) {

    FACELOGIN_INFO(L"=== ReportResult ENTER (status=0x%08X, substatus=0x%08X, state=%d) ===",
                  ntsStatus, ntsSubstatus, static_cast<int>(m_state));

    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    if (ntsStatus == STATUS_SUCCESS) {
        FACELOGIN_INFO(L"Authentication succeeded");
    } else {
        FACELOGIN_WARN(L"Authentication failed: status=0x%08X, substatus=0x%08X",
                      ntsStatus, ntsSubstatus);

        // A rejected stored credential is terminal for this face attempt.
        // Returning to Waiting would re-enable passive input detection after
        // LogonUI re-advises the tile and could again consume password-entry
        // keystrokes as face retries. Require an explicit retry instead.
        facelogin::SecureClearWString(m_password);
        PresentRetryableFailure(
            State::Failed,
            L"Windows 拒绝了保存的凭据，请使用密码登录并重新录入人脸");

        if (m_pipeClient) {
            m_pipeClient.reset();
        }
    }

    return S_OK;
}

// ============================================================================
// Private: StartAuth — begin the authentication pipeline
// ============================================================================

void FaceLoginCredential::StartAuth() {
    // CRITICAL SECTION: m_cs is held by the caller (if from background
    // thread). The pipe callbacks also acquire m_cs, so we must NOT hold
    // it here. In our design, only the main thread (Advise) and the
    // input-detection thread call StartAuth, and they do so outside m_cs.
    FACELOGIN_INFO(L"StartAuth: connecting to face service pipe (state=%d)", static_cast<int>(m_state));

    if (m_pipeClient && m_pipeClient->IsConnected()) {
        FACELOGIN_INFO(L"StartAuth: already connected, skipping");
        return;
    }

    m_state = State::Authenticating;
    m_pipeClient = std::make_unique<facelogin::PipeClient>();

    if (m_pipeClient->Connect()) {
        if (!m_pipeClient->SendMessage(facelogin::ipc::MSG_AUTH_REQUEST)) {
            FACELOGIN_WARN(L"Failed to send authentication request");
            PresentRetryableFailure(State::Error, L"人脸登录服务不可用");
            return;
        }

        // Push "正在识别..." immediately so the tile does not keep showing the
        // previous content (last round's failure text or the idle prompt)
        // during the pipe round-trip before the service's first STATUS
        // message arrives — that gap reads as a brief flash of stale text.
        m_statusText = L"正在识别...";
        if (m_pCredentialEvents) {
            m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        }

        auto self = this;
        m_pipeClient->StartBackgroundRead(
            [self](bool success, const std::wstring& msg) {
                self->OnPipeResponse(success, msg);
            },
            [self](const std::wstring& msg) {
                self->OnPipeStatus(msg);
            });
        FACELOGIN_INFO(L"Pipe connected, auth request sent");
    } else {
        FACELOGIN_WARN(L"Failed to connect to face service pipe");
        PresentRetryableFailure(State::Error, L"人脸登录服务不可用");
    }
}

// ============================================================================
// Private: StartInputDetectionThread / StopInputDetectionThread
// ============================================================================

void FaceLoginCredential::StartInputDetectionThread() {
    if (m_inputThreadRunning) {
        FACELOGIN_WARN(L"StartInputDetectionThread: thread already running");
        return;
    }

    if (!m_hInputStop) {
        m_hInputStop = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_hInputStop) {
            FACELOGIN_ERROR(L"Failed to create input stop event");
            return;
        }
    } else {
        ResetEvent(m_hInputStop);
    }

    auto* ctx = new InputDetectionContext;
    ctx->pCred = this;

    m_inputThreadRunning = true;
    unsigned threadId = 0;
    m_hInputThread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, InputDetectionThreadProc, ctx, 0, &threadId));
    if (!m_hInputThread || m_hInputThread == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"Failed to start input detection thread");
        m_inputThreadRunning = false;
        delete ctx;
    } else {
        FACELOGIN_INFO(L"Input detection thread started (id=%u)", threadId);
    }
}

void FaceLoginCredential::StopInputDetectionThread() {
    if (!m_inputThreadRunning) {
        return;
    }

    FACELOGIN_INFO(L"Stopping input detection thread...");

    // Signal stop
    if (m_hInputStop) {
        SetEvent(m_hInputStop);
    }

    // Wait for thread to exit (up to 2 seconds)
    if (m_hInputThread) {
        DWORD waitResult = WaitForSingleObject(m_hInputThread, 2000);
        if (waitResult == WAIT_TIMEOUT) {
            FACELOGIN_WARN(L"Input thread did not stop within 2s — terminating");
            TerminateThread(m_hInputThread, 0);
        }
        CloseHandle(m_hInputThread);
        m_hInputThread = nullptr;
    }

    if (m_hInputStop) {
        CloseHandle(m_hInputStop);
        m_hInputStop = nullptr;
    }

    m_inputThreadRunning = false;
    FACELOGIN_INFO(L"Input detection thread stopped");
}

// ============================================================================
// Private: Credential Packing
// ============================================================================

HRESULT FaceLoginCredential::PackCredentials(
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) {

    // RAII: guarantee m_password is zeroed on EVERY return path, including
    // the three early-returns below (CredPack query failure / OOM / pack
    // failure). Previously only the success path cleared it, so a packing
    // failure left plaintext in the member wstring until the destructor —
    // and LogonUI re-polls GetSerialization, so the password sat there for
    // the credential's lifetime (security #9).
    facelogin::SecureWStringGuard pwdGuard(m_password);

    FACELOGIN_INFO(L"Packing credentials for: %s\\%s (UPN=%s)",
                  m_domain.c_str(), m_username.c_str(),
                  m_upn.empty() ? L"<none>" : m_upn.c_str());

    // Auth package: MSV1_0 for LOGON/UNLOCK.
    // (CredUI/PLAP never reach here — they're filtered in SetUsageScenario.)
    ULONG ulAuthPackage = 0;
    HANDLE hLsa = nullptr;
    NTSTATUS lsastatus = LsaConnectUntrusted(&hLsa);
    if (lsastatus == 0 && hLsa) {
        LSA_STRING pkgName;
        char msvStr[] = "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0";
        pkgName.Buffer = msvStr;
        pkgName.Length = static_cast<USHORT>(strlen(msvStr));
        pkgName.MaximumLength = pkgName.Length;
        lsastatus = LsaLookupAuthenticationPackage(hLsa, &pkgName, &ulAuthPackage);
        if (lsastatus != 0) {
            FACELOGIN_ERROR(L"LsaLookupAuthenticationPackage MSV1_0 failed: 0x%08X", lsastatus);
            ulAuthPackage = 0;
        }
        LsaDeregisterLogonProcess(hLsa);
    } else {
        FACELOGIN_ERROR(L"LsaConnectUntrusted failed: 0x%08X", lsastatus);
    }
    FACELOGIN_INFO(L"Auth package MSV1_0: %lu", ulAuthPackage);

    DWORD packFlags = 0;
    DWORD cbPackedCreds = 0;

    PWSTR pwzPassword = const_cast<PWSTR>(m_password.c_str());

    // Build the packed user name in the correct format.
    // Local/domain: "DOMAIN\Username" (required by CredPackAuthenticationBuffer)
    // MSA/AAD:      UPN "user@domain.com"
    std::wstring packedUser;
    if (!m_upn.empty() && m_upn.find(L'@') != std::wstring::npos) {
        packedUser = m_upn;
    } else {
        packedUser = m_domain + L"\\" + m_username;
    }
    FACELOGIN_INFO(L"CredPack user: \"%s\"", packedUser.c_str());

    if (!CredPackAuthenticationBufferW(
            packFlags,
            const_cast<LPWSTR>(packedUser.c_str()),
            pwzPassword,
            nullptr,
            &cbPackedCreds)) {

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            FACELOGIN_ERROR(L"CredPackAuthenticationBuffer size query failed: %lu",
                           GetLastError());
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }

    // Allocate buffer and pack
    BYTE* pPackedCreds = static_cast<BYTE*>(CoTaskMemAlloc(cbPackedCreds));
    if (!pPackedCreds) {
        return E_OUTOFMEMORY;
    }

    if (!CredPackAuthenticationBufferW(
            packFlags,
            const_cast<LPWSTR>(packedUser.c_str()),
            pwzPassword,
            pPackedCreds,
            &cbPackedCreds)) {
        FACELOGIN_ERROR(L"CredPackAuthenticationBuffer failed: %lu", GetLastError());
        CoTaskMemFree(pPackedCreds);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Password clearing is handled by pwdGuard on return (success path
    // included) — no manual SecureZeroMemory needed here.

    pcpcs->rgbSerialization = pPackedCreds;
    pcpcs->cbSerialization = cbPackedCreds;
    pcpcs->ulAuthenticationPackage = ulAuthPackage;
    pcpcs->clsidCredentialProvider = CLSID_FaceLoginProvider;

    FACELOGIN_INFO(L"Credentials packed successfully (%lu bytes, pkg=%lu)",
                   cbPackedCreds, ulAuthPackage);
    return S_OK;
}

// ============================================================================
// Private: Switch to Password Provider
// ============================================================================

HRESULT FaceLoginCredential::SwitchToPasswordProvider() {
    // Set the terminal state before notifying LogonUI. CredentialsChanged may
    // synchronously cause UnAdvise/Advise; Advise must already see Failed so
    // it cannot restart passive face authentication.
    m_state = State::Failed;

    // Signal LogonUI to re-enumerate credentials
    // The user can then select the password provider
    if (m_pProviderEvents) {
        m_pProviderEvents->CredentialsChanged(m_upAdviseContext);
    }

    // Also return NO_CREDENTIAL_FINISHED to deselect our tile
    // This causes LogonUI to show other providers
    // (Actually done in GetSerialization via state change)
    return S_OK;
}

void FaceLoginCredential::OnPipeStatus(const std::wstring& message) {
    EnterCriticalSection(&m_cs);
    m_statusText = message;
    LeaveCriticalSection(&m_cs);
    FACELOGIN_INFO(L"Status text updated: %s", message.c_str());
    // Use SetFieldString to update the status text in-place on the lock
    // screen, without triggering re-enumeration (which destroys the pipe).
    if (m_pCredentialEvents) {
        m_pCredentialEvents->SetFieldString(this, 1, message.c_str());
    }
}

void FaceLoginCredential::OnPipeResponse(bool success, const std::wstring& message) {
    if (success) {
        auto result = facelogin::ipc::ParseAuthMessage(message);

        if (result.status == facelogin::ipc::AuthResult::Status::Success) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth success: domain=%s, username=%s (SID=%s, UPN=%s)",
                          result.domain.c_str(), result.username.c_str(),
                          result.sid.c_str(), result.upn.c_str());
            // NOTE: the password itself is never logged — only metadata.
            m_upn = result.upn;
            m_domain = result.domain;
            m_username = result.username;
            m_password = result.password;
            m_state = State::Ready;
            // Push the success text immediately so the tile does not keep
            // showing the last in-flight status ("正在识别...") during
            // the re-enumeration gap before LogonUI calls GetStringValue.
            m_statusText = L"人脸识别成功，正在解锁...";
            if (m_pCredentialEvents) {
                m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
            }
            if (m_hCredsReady) {
                SetEvent(m_hCredsReady);
            }
            // Ask LogonUI to call GetSerialization again right away
            if (facelogin::credential_provider::ShouldReenumerateAfterTerminal(m_state)) {
                TriggerReEnumeration();
            }
            return;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Timeout) {
            FACELOGIN_INFO(L"OnPipeResponse: Auth timeout");
            PresentRetryableFailure(State::Failed, L"识别超时，请重试");
            return;
        } else if (result.status == facelogin::ipc::AuthResult::Status::Error) {
            FACELOGIN_WARN(L"OnPipeResponse: Auth error: %s", result.errorMessage.c_str());
            // Surface the service's specific error (e.g. "检测到攻击，请使用真实人脸")
            // on the lock screen instead of the generic "service unavailable".
            PresentRetryableFailure(
                State::Error,
                result.errorMessage.empty()
                    ? L"人脸登录服务不可用"
                    : result.errorMessage);
            return;
        }
    } else {
        FACELOGIN_WARN(L"OnPipeResponse: Read failed — server disconnected?");
        PresentRetryableFailure(State::Error, L"人脸登录服务不可用");
        return;
    }
}

void FaceLoginCredential::PresentRetryableFailure(
    State failureState,
    const std::wstring& statusText) {
    if (!facelogin::credential_provider::IsRetryableFailure(failureState)) {
        FACELOGIN_ERROR(L"PresentRetryableFailure called with non-failure state=%d",
                        static_cast<int>(failureState));
        return;
    }

    m_state = failureState;
    m_authStartTime = 0;
    m_statusText = statusText.empty() ? L"人脸识别失败，请重试或使用密码登录"
                                      : statusText;

    // Update the selected tile in-place. In particular, do not call
    // CredentialsChanged: that causes UnAdvise/Advise and used to restart the
    // global input watcher while the user was typing a password.
    if (m_pCredentialEvents) {
        m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        m_pCredentialEvents->SetFieldString(this, 3, L"重新尝试人脸识别");
        m_pCredentialEvents->SetFieldState(this, 3, CPFS_DISPLAY_IN_SELECTED_TILE);
    }
    FACELOGIN_INFO(L"Terminal failure shown in-place; passive retry disabled");
}

void FaceLoginCredential::StartExplicitRetry() {
    if (!facelogin::credential_provider::IsRetryableFailure(m_state)) {
        return;
    }

    // The previous read thread has already delivered its terminal response.
    // Destroying the client joins that completed thread and guarantees the new
    // request cannot reuse a terminal pipe connection.
    m_pipeClient.reset();
    m_authStartTime = 0;
    m_statusText = L"正在识别...";
    m_state = State::Waiting;

    if (m_pCredentialEvents) {
        m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        m_pCredentialEvents->SetFieldString(this, 3, L"切换到密码登录");
        m_pCredentialEvents->SetFieldState(this, 3, CPFS_DISPLAY_IN_DESELECTED_TILE);
    }

    // This path is intentionally direct. Re-enabling passive key/mouse polling
    // would again make password input ambiguous; clicking the command is the
    // explicit user intent to retry face authentication.
    StartAuth();
}

// ============================================================================
// Private: Trigger Re-enumeration
// ============================================================================

void FaceLoginCredential::TriggerReEnumeration() {
    if (m_pProviderEvents) {
        FACELOGIN_DEBUG(L"Triggering CredentialsChanged");
        m_pProviderEvents->CredentialsChanged(m_upAdviseContext);
    }
}
