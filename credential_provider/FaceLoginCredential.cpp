#include "FaceLoginCredential.h"
#include "FaceLoginProvider.h"
#include "resource.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include "../common/secure_clear.h"
#include <wincred.h>
// winnt.h (via windows.h above) and ntstatus.h both define the STATUS_*
// constants with identical values; silence the benign C4005 redefinitions.
#pragma warning(push)
#pragma warning(disable : 4005)
#include <ntstatus.h>
#pragma warning(pop)
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
// The SAME watcher, with the SAME qualifying-wave gate, is re-armed for the
// in-place failure tile (ArmFailureRetryDetection): after a terminal
// failure any qualifying press (key or mouse button) requests another face
// round via StartExplicitRetry. The failure tile carries NO command link —
// the affordance is field 4 ("请按任意键重试", its own field because tile
// text ignores "\r\n"). Password entry stays structurally excluded: this tile has no editable field, and
// SetDeselected stops the watcher before the user can type anywhere else.
// One deliberate hole: when the triggered round fails IMMEDIATELY on the
// watcher thread itself (pipe connect/send error → "人脸登录服务不可用"),
// ArmFailureRetryDetection no-ops because that thread still occupies the
// running slot and nothing re-arms after it exits — acceptable because a
// dead service makes press-to-retry useless noise anyway; re-selecting the
// tile (SetSelected → ArmFailureRetryDetection) re-arms once the service
// recovers.
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
//   Wave + quiesce. A new input tick starts a wave. A wave QUALIFIES only
//   if evidence of a real key press or mouse click arrived while it ran.
//   Final product semantics (user decision 2026-08-29): any keyboard key
//   triggers with ONE press — even a quick tap on the wallpaper — via the
//   raw-input KEYUP evidence below; a mouse CLICK takes two (first click
//   wakes the wallpaper, second click triggers once the credential view is
//   up, where GetAsyncKeyState finally sees mouse buttons); a mouse MOVE
//   never triggers. GetLastInputInfo cannot tell input types apart, and on
//   Win11 a mere mouse MOVE dismisses the lock-screen wallpaper, so without
//   the qualifier brushing the mouse would auto-start face recognition
//   against the tile's own "按下任意按键" prompt (the original 2026-08-29
//   bug).
//
//   Secure-desktop channel matrix (装机实测 2026-08-29, do NOT retry
//   these): GetAsyncKeyState sees keyboard keys AND mouse buttons only
//   once the credential view is up, never during wallpaper dismissal;
//   WH_MOUSE_LL installs but the system never dispatches callbacks in
//   LogonUI; Raw Input (RIDEV_INPUTSINK on a message-only window owned by
//   this thread) works, and during wallpaper dismissal the system swallows
//   the KEYDOWN half of a key press — but the matching KEYUP leaks through,
//   which is exactly the one-press keyboard trigger — while a whole mouse
//   click (BUTTON_DOWN and BUTTON_UP) is swallowed with nothing leaking,
//   which is why a one-press click trigger is physically impossible.
//
//   Once no further tick arrives for QUIESCE_MS AND no key is currently
//   physically held (a held key keeps generating auto-repeat ticks, so it
//   must never count as a finished wave), the wave has ended: a
//   qualifying wave fires StartAuth() IMMEDIATELY — the user's single
//   dismiss PRESS is the trigger (trade-off: there is no way to "just
//   dismiss" with a press without starting face recognition) — while a
//   movement-only wave is discarded and the watcher keeps waiting:
//   moving the mouse wakes the screen but never opens the camera. QUIESCE_MS only needs to clear the keyboard
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
//     waveQualifying    — a keyboard key press was observed during this wave
//                          (monotonic within a wave: never reset on new
//                          ticks — a KEYUP tick would otherwise erase the
//                          qualifying press that preceded it; only reset
//                          when a wave is discarded or the window restarts)
//     armed             — true once the dismiss wave has gone quiet for
//                          QUIESCE_MS; on becoming true we fire StartAuth()
//                          right away and exit the loop.
//
// The thread stops when:
//   - The dismiss wave goes quiet and StartAuth() is auto-triggered, OR
//   - The stop event is signaled (UnAdvise / destructor), OR
//   - UnAdvise() sets m_pCredentialEvents = nullptr and the thread notices

// Keys still held at the baseline moment are the mid-gesture fingerprint of
// a lock action (see the algorithm comment above). Any key counts — see the
// Win-cleared observation there for why modifiers alone are not enough.
static const int kSnapshotFirstVk = 0x01;   // VK_LBUTTON
static const int kSnapshotLastVk = 0xFE;    // VK_OEM_CLEAR

void FaceLoginCredential::SnapshotBaselineKeys() {
    m_baselineKeysHeld.clear();
    for (int vk = kSnapshotFirstVk; vk <= kSnapshotLastVk; ++vk) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            m_baselineKeysHeld.push_back(vk);
        }
    }
}

// True while any key or mouse button is physically held. Used at arm time so
// a held (auto-repeating) key can never end a wave — its eventual KEYUP
// restarts the quiet window instead, regardless of the repeat interval.
// NOTE: inside LogonUI's secure desktop this sees keyboard keys and mouse
// buttons ONLY once the credential view is up — never during wallpaper
// dismissal (装机实测 2026-08-29; see the trigger algorithm comment for the
// full channel matrix).
static bool AnyKeyPhysicallyDown() {
    for (int vk = kSnapshotFirstVk; vk <= kSnapshotLastVk; ++vk) {
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    }
    return false;
}

// Keyboard press evidence from Raw Input since the last pump. The sink window
// belongs to the input-detection thread and WM_INPUT is dispatched only while
// that same thread pumps — no lock needed.
//
// This is the ONLY channel that sees a key press while the lock-screen
// wallpaper is dismissing: the system swallows the KEYDOWN half of the
// dismiss gesture on every side channel, but the matching KEYUP leaks through
// Raw Input (装机实测 2026-08-29). An UP alone proves a complete press, so
// both MAKE and BREAK qualify the wave — that is what makes a quick tap on
// the wallpaper trigger auth with ONE press. Residue safety: a key still
// held at baseline is captured by the Advise snapshot and its trailing UP is
// swallowed by the residue-quarantine watermark; a key released before
// baseline never forms a wave. Either way an UP can only qualify a genuine
// post-baseline press.
//
// Keyboard ONLY. Mouse is deliberately NOT registered: during wallpaper
// dismissal the system swallows the whole click (BUTTON_DOWN and BUTTON_UP,
// same 实测), so a first-press click trigger is physically impossible and
// the mouse stays at "first click wakes, second click triggers" via the
// GetAsyncKeyState scan (mouse buttons are visible to it once the
// credential view is up).
static thread_local LONG t_rawKeyboardSeen = 0;

static LRESULT CALLBACK RawInputSinkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RAWINPUT raw = {};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                            &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            raw.header.dwType == RIM_TYPEKEYBOARD) {
            // Both MAKE and BREAK qualify the wave — an UP alone proves a
            // complete press happened, so the release flag needs no handling.
            t_rawKeyboardSeen = 1;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct RawInputSink {
    HWND hwnd = nullptr;
    ATOM classAtom = 0;
    HINSTANCE hInst = nullptr;
    wchar_t className[64] = {};

    bool Register();
    void Unregister() noexcept;
};

bool RawInputSink::Register() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&RawInputSinkWndProc), &hInst);
    wsprintfW(className, L"FaceLoginRawSink_%08lx", GetCurrentThreadId());

    WNDCLASSW wc = {};
    wc.lpfnWndProc = RawInputSinkWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = className;
    classAtom = RegisterClassW(&wc);
    if (!classAtom) {
        FACELOGIN_WARN(L"[InputThread] raw-input class registration failed (GLE=%lu)",
                       GetLastError());
        return false;
    }

    // HWND_MESSAGE parent: invisible message-only window, exactly what a
    // RIDEV_INPUTSINK registration needs as its delivery address.
    hwnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        FACELOGIN_WARN(L"[InputThread] raw-input sink window creation failed (GLE=%lu)",
                       GetLastError());
        Unregister();
        return false;
    }

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;         // generic desktop
    rid.usUsage = 0x06;             // keyboard
    rid.dwFlags = RIDEV_INPUTSINK;  // deliver even without foreground
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        FACELOGIN_WARN(L"[InputThread] RegisterRawInputDevices failed (GLE=%lu) — "
                       L"key presses degrade to credential-view-only detection",
                       GetLastError());
        Unregister();
        return false;
    }
    return true;
}

void RawInputSink::Unregister() noexcept {
    if (hwnd) {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x06;
        rid.dwFlags = RIDEV_REMOVE;  // requires hwndTarget == nullptr
        rid.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (classAtom) {
        UnregisterClassW(className, hInst);
        classAtom = 0;
    }
}

unsigned __stdcall FaceLoginCredential::InputDetectionThreadProc(void* pParam) {
    FaceLoginCredential* pCred = static_cast<FaceLoginCredential*>(pParam);

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
    bool waveQualifying = false;        // wave contains a real key/button press
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
    }

    // The keyboard raw-input sink is created AFTER residue quarantine (which
    // does not pump messages — a WM_INPUT arriving meanwhile just queues up
    // and the first main-loop pump consumes it) and before the wait loop, so
    // every wait is a message-pumping wait that dispatches WM_INPUT.
    RawInputSink rawSink;
    rawSink.Register();

    // Loop forever (until the stop event is signaled).  The 30s timeout does
    // NOT kill the thread — it only restarts the idle window so a user who
    // waits longer than 30s before pressing a key can still trigger auth.
    while (true) {
        // Check stop signal (non-blocking)
        DWORD waitResult = WaitForSingleObject(pCred->m_hInputStop, 0);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }

        // Idle-window timeout: restart the window instead of exiting, so a
        // late keypress still works. (Previously the thread exited after 30s
        // of no input, leaving no path to restart it — a later keypress did
        // nothing.)  Also reset the adaptive state so the next wave is
        // evaluated cleanly.
        DWORD elapsedMs = GetTickCount() - startTick;
        if (elapsedMs > timeoutSec * 1000) {
            startTick = GetTickCount();
            baseline = pCred->m_waitingStartTick;
            lastInputTick = baseline;
            lastInputEndWall = 0;
            waveQualifying = false;
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
                // the wave goes quiet, then auto-trigger. waveQualifying is
                // deliberately NOT reset here — see its state comment above.
                lastInputTick = lii.dwTime;
                lastInputEndWall = GetTickCount();
                armed = false;
            }

            // Qualifier scan, every cycle: a press is observable via
            // GetAsyncKeyState only while physically held, which frequently
            // falls between the ticks of one poll interval. Inside LogonUI
            // this sees keyboard keys and mouse buttons once the credential
            // view is up (the mouse's second-click trigger rides on this).
            if (AnyKeyPhysicallyDown()) {
                waveQualifying = true;
            }

            // Arm once the current wave has been quiet for QUIESCE_MS AND no
            // key is physically held (a held key is still generating its
            // wave). lastInputEndWall==0 means we've never seen a
            // post-baseline input yet, so there's nothing to arm against.
            if (!armed && lastInputEndWall != 0 &&
                GetTickCount() - lastInputEndWall >= QUIESCE_MS &&
                !AnyKeyPhysicallyDown()) {
                if (!waveQualifying) {
                    // Movement-only wave (e.g. the mouse move that dismissed
                    // the wallpaper): discard it and keep waiting — the tile
                    // still says "按下任意按键". Zeroing lastInputEndWall
                    // makes this branch run once per discarded wave.
                    lastInputEndWall = 0;
                    waveQualifying = false;
                } else {
                    armed = true;
                    // Auto-trigger: the dismiss wave (KEYDOWN+KEYUP) has fully
                    // ended, so the user's single dismiss press is enough.
                    // Start auth immediately instead of waiting for another wave.
                    // Failure-tile round: route through the explicit-retry
                    // entry so the terminal pipe from the failed round is
                    // destroyed first (a direct StartAuth could see the dead
                    // client as "already connected" and skip, or reuse it).
                    // First-attempt round: straight to StartAuth.
                    if (facelogin::credential_provider::IsRetryableFailure(
                            pCred->m_state)) {
                        FACELOGIN_INFO(L"[InputThread] Qualifying press on "
                                      L"failure tile — requesting explicit "
                                      L"retry");
                        pCred->StartExplicitRetry();
                    } else {
                        pCred->StartAuth();
                    }
                    break;
                }
            }
        }

        // Sleep (alertable so the stop event can wake us). While a wave is
        // being timed out, wake exactly when the quiet window can complete
        // instead of drifting up to a full poll interval past it. This is a
        // message-pumping wait: DispatchMessage routes WM_INPUT to the sink
        // wndproc, whose key-press flag qualifies the current wave. The pump
        // runs even when the sink failed to register — it is then an empty
        // no-op and the wait is just an alertable sleep.
        DWORD sleepMs = pollIntervalMs;
        if (lastInputEndWall != 0) {
            const DWORD quietSoFar = GetTickCount() - lastInputEndWall;
            if (quietSoFar < QUIESCE_MS && QUIESCE_MS - quietSoFar < sleepMs) {
                sleepMs = QUIESCE_MS - quietSoFar + 1;
            }
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }
        if (InterlockedExchange(&t_rawKeyboardSeen, 0) != 0) {
            waveQualifying = true;
        }
        DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &pCred->m_hInputStop, sleepMs, QS_ALLINPUT,
            MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
    }

    rawSink.Unregister();  // idempotent no-op if registration failed
    pCred->m_inputThreadRunning = false;
    return 0;
}

// Idle prompt shown on the Waiting tile — one constant so the deselect reset
// and GetStringValue can never drift apart.
static const wchar_t kWaitingPrompt[] = L"按下任意按键以开始人脸识别";

// ============================================================================
// Construction / Destruction
// ============================================================================

FaceLoginCredential::FaceLoginCredential() {
    InitializeCriticalSection(&m_cs);
    m_csInitialized = true;

    FACELOGIN_DEBUG(L"FaceLoginCredential created");
}

FaceLoginCredential::~FaceLoginCredential() {
    // SENSITIVE: Zero the password from memory
    facelogin::SecureClearWString(m_password);
    // ... and the retained packed credential (it carries the same plaintext)
    ClearPackedCredentials();

    // Stop the background input-detection thread before tearing down any state
    // it touches. COM release order does not guarantee UnAdvise (which also
    // calls StopInputDetectionThread) runs before the destructor — if the
    // thread is still running when `this` is freed, its next access to pCred
    // is a use-after-free. Idempotent and safe to call when not running.
    StopInputDetectionThread();

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

    // A terminal failure must NOT (re)start the watcher from Advise:
    // LogonUI calls Advise for every enumeration — including flows that
    // never select this tile (PIN reset wizard) and moments when another
    // tile is focused — and the watcher polls GLOBAL input, so starting it
    // here would interpret password-entry keystrokes elsewhere as face
    // retries. The failure watcher is armed only from
    // PresentRetryableFailure/SetSelected, i.e. strictly while this tile is
    // the selected one. No baseline is seeded here either —
    // ArmFailureRetryDetection seeds its own.
    if (facelogin::credential_provider::IsRetryableFailure(m_state)) {
        FACELOGIN_INFO(L"Advise: terminal failure — retry watcher NOT started "
                       L"here (armed tile-scoped instead)");
        return S_OK;
    }

    // Guard: if we're already authenticating and have a live pipe,
    // don't create a second connection.
    if (m_state == State::Authenticating && m_pipeClient && m_pipeClient->IsConnected()) {
        FACELOGIN_INFO(L"Advise: already authenticating, skipping auth restart");
        return S_OK;
    }

    // Activation lives in SetSelected, NOT here: LogonUI calls Advise for
    // every credential enumeration — including flows that never select this
    // tile (the MSA PIN reset wizard reuses the LogonUI credential list) —
    // and the input watcher polls GLOBAL input, so starting it at Advise let
    // typing in those flows trigger the camera and interrupt the wizard.
    // Advise still records the baseline tick and held-key snapshot as early
    // as possible; SetSelected starts the watcher only when the user
    // actually lands on this tile. (CredUI/PLAP never reach this point —
    // SetUsageScenario already returned E_NOTIMPL for them.)
    if (!facelogin::credential_provider::ShouldStartInputDetection(m_state)) {
        FACELOGIN_INFO(L"Advise: state does not permit passive input detection");
        return S_OK;
    }
    m_waitingStartTick = GetTickCount();
    FACELOGIN_INFO(L"Advise: baseline tick = %lu", m_waitingStartTick);
    // Same instant as the baseline tick: capture which lock-shortcut
    // modifiers are still physically held, before the input thread starts.
    SnapshotBaselineKeys();

    return S_OK;
}

STDMETHODIMP FaceLoginCredential::UnAdvise() {
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
    if (m_state == State::Ready) {
        // Credentials ready (bg thread finished auth):
        // enable auto-logon so LogonUI calls GetSerialization to pack creds.
        *pbAutoLogon = TRUE;
    } else {
        // Still Waiting: no auto-logon; we wait for the bg thread.
        *pbAutoLogon = FALSE;
    }

    // The user actually landed on this tile (LogonUI focuses it right after
    // the Advise pass of the same enumeration round — milliseconds apart),
    // so starting the passive input watcher here keeps the keypress→auth
    // latency identical while never listening during flows that only Advise
    // us (PIN reset wizard, other tiles focused). Re-selecting after a
    // deselect restarts the watcher the same way; a re-selected FAILED tile
    // re-arms the failure watcher (fresh baseline — Advise did not seed one
    // in failure state), so the residue quarantine also applies to the
    // key/click that performed the re-selection itself.
    if (facelogin::credential_provider::ShouldStartInputDetection(m_state) &&
        !m_inputThreadRunning) {
        if (facelogin::credential_provider::IsRetryableFailure(m_state)) {
            FACELOGIN_INFO(L"SetSelected: re-arming passive retry detection");
            ArmFailureRetryDetection();
        } else {
            FACELOGIN_INFO(L"SetSelected: starting passive input detection");
            StartInputDetectionThread();
        }
    }

    return S_OK;
}

STDMETHODIMP FaceLoginCredential::SetDeselected() {
    // The user moved to another sign-in option: stop watching global input,
    // and if a recognition is in flight, tear the pipe down — the service
    // observes the client disconnect, aborts the loop and releases the
    // camera instead of filming until the auth timeout. Order matters: stop
    // the input thread FIRST (it joins), so a concurrent auto-trigger cannot
    // start a new pipe after the teardown below.
    StopInputDetectionThread();

    if (facelogin::credential_provider::ShouldAbortAuthOnDeselect(m_state)) {
        FACELOGIN_INFO(L"SetDeselected: aborting in-flight authentication");
        m_pipeClient.reset();
        SetStatusText(kWaitingPrompt);
        m_state = State::Waiting;
        if (m_pCredentialEvents) {
            m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        }
        // Late pipe results are dropped by the ShouldProcessPipeResponse
        // guard in OnPipeResponse/OnPipeStatus; re-selecting the tile
        // restarts the watcher through SetSelected.
    }
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — Field State / Values
// ============================================================================

STDMETHODIMP FaceLoginCredential::GetFieldState(
    DWORD dwFieldID,
    CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) {

    *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
    *pcpfis = CPFIS_NONE;

    switch (dwFieldID) {
    case 0: // "Face Login" label — kept on the failure tile too (user
        // decision 2026-08-29: a title-less tile reads worse than a
        // three-line one; line count was never the complaint).
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case 1: // Status text — always visible so error states are seen
        *pcpfs = CPFS_DISPLAY_IN_BOTH;
        break;

    case 2: // Submit button — hidden in both scenarios
        *pcpfs = CPFS_HIDDEN;
        break;

    case 3: // Password-switch link, deselected tile-list view only. The
        // failure tile carries NO link: retries there are input-triggered
        // (ArmFailureRetryDetection) and field 4 carries the hint.
        *pcpfs = facelogin::credential_provider::IsRetryableFailure(m_state)
            ? CPFS_HIDDEN
            : CPFS_DISPLAY_IN_DESELECTED_TILE;
        break;

    case 4: // "请按任意键重试" hint line under the failure reason (its own
        // field because tile text fields ignore "\r\n" — 装机实测
        // 2026-08-29). Enumeration shows it for any retryable failure:
        // a re-selected tile has SetSelected-armed detection, so the hint
        // is truthful by the time it renders. The one unarmed case (dead
        // service) hides it in-place from PresentRetryableFailure.
        *pcpfs = facelogin::credential_provider::IsRetryableFailure(m_state)
            ? CPFS_DISPLAY_IN_SELECTED_TILE
            : CPFS_HIDDEN;
        break;

    default:
        return E_INVALIDARG;
    }

    return S_OK;
}

STDMETHODIMP FaceLoginCredential::GetStringValue(DWORD dwFieldID, PWSTR* ppwsz) {
    *ppwsz = nullptr;

    switch (dwFieldID) {
    case 0: // Label
        return SHStrDupW(L"人脸登录", ppwsz);

    case 1: // Status
        switch (m_state) {
        case State::Waiting:
            return SHStrDupW(kWaitingPrompt, ppwsz);
        case State::Ready:
            return SHStrDupW(L"人脸识别成功，正在解锁...", ppwsz);
        case State::Authenticating:
        case State::Failed:
        case State::Error: {
            // Surface the specific service-provided text when one exists
            // (live "正在识别...", failure reasons like "未检测到人脸",
            // service errors like "人脸登录服务不可用"); otherwise the
            // per-state generic fallback. Snapshot under m_cs — the text is
            // concurrently written by the pipe read thread.
            const std::wstring snapshot = SnapshotStatusText();
            if (!snapshot.empty()) {
                return SHStrDupW(snapshot.c_str(), ppwsz);
            }
            return SHStrDupW(m_state == State::Authenticating
                                 ? L"正在识别..."
                                 : m_state == State::Failed
                                       ? L"人脸识别失败"
                                       : L"人脸登录服务不可用",
                             ppwsz);
        }
        default:
            return SHStrDupW(L"", ppwsz);
        }

    case 2: // Submit button
        return SHStrDupW(L"", ppwsz);

    case 3: // Command link — password switch, deselected list view only
        return SHStrDupW(L"切换到密码登录", ppwsz);

    case 4: // Retry hint line (visibility is state-driven, see GetFieldState)
        return SHStrDupW(L"请按任意键重试", ppwsz);

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
            // UI-unreachable since the failure tile hides field 3 — kept as
            // defense (a hidden command link must still do the right thing
            // if LogonUI ever dispatches it, e.g. via keyboard focus).
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

    if (m_state == State::Authenticating) {
        // Terminal results arrive exclusively via OnPipeResponse on the pipe
        // read thread (a dead service breaks the pipe and is reported there
        // within one 50 ms poll). This polling-side guard only bounds the
        // rare case where LogonUI polls us while Authenticating (e.g. the
        // user pressed Enter) and the service never answers at all.
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
        // Not ready yet — LogonUI re-polls GetSerialization when auto-logon
        // is set; OnPipeResponse flips the state and re-enumerates.
        return S_OK;
    }

    // State::Ready — hand LogonUI the credential. The FIRST call packs
    // fresh (retaining a copy in m_packedCreds, because m_password is
    // zeroed on every pack return — see PackCredentials); any later call
    // re-serves from that retained copy.
    if (m_packedCreds) {
        BYTE* copy = static_cast<BYTE*>(CoTaskMemAlloc(m_cbPackedCreds));
        if (!copy) {
            FACELOGIN_ERROR(L"GetSerialization: CoTaskMemAlloc for cached credential failed");
            return E_OUTOFMEMORY;
        }
        memcpy(copy, m_packedCreds, m_cbPackedCreds);
        pcpcs->rgbSerialization = copy;
        pcpcs->cbSerialization = m_cbPackedCreds;
        pcpcs->ulAuthenticationPackage = m_ulAuthPackage;
        pcpcs->clsidCredentialProvider = CLSID_FaceLoginProvider;
        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
        return S_OK;
    }

    if (m_password.empty()) {
        // Ready without credentials and without a retained pack cannot
        // happen (Ready is only entered with a full result) — fail closed
        // rather than spinning NOT_FINISHED forever.
        FACELOGIN_ERROR(L"GetSerialization: Ready state has no credentials to serialize");
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
        return S_OK;
    }

    // NOTE: never log the password or any part of it — it is a credential.
    HRESULT hr = PackCredentials(pcpcs);
    if (SUCCEEDED(hr)) {
        *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
    } else {
        FACELOGIN_ERROR(L"PackCred FAILED: hr=0x%08X", hr);
        // A failed pack leaves no retained copy and the password is already
        // zeroed — later polls could not succeed either. Present a terminal
        // failure instead of spinning NOT_FINISHED.
        PresentRetryableFailure(State::Error, L"凭据封装失败，请使用密码登录");
        *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED;
    }
    return S_OK;
}

// ============================================================================
// ICredentialProviderCredential — ReportResult
// ============================================================================

STDMETHODIMP FaceLoginCredential::ReportResult(
    NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
    PWSTR* ppwszOptionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) {

    *ppwszOptionalStatusText = nullptr;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    // The handoff is over (accepted or rejected) — the retained packed
    // credential must not outlive it.
    ClearPackedCredentials();

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
    // Single reset point for the polling-side deadline: every round (first
    // attempt, explicit retry, re-armed watcher trigger) enters here, so a
    // round can never inherit the start time of an earlier, aborted one and
    // be instantly judged "timed out" by GetSerialization.
    m_authStartTime = 0;

    // StartAuth runs on the input-detection thread (auto-trigger / explicit
    // retry) or the LogonUI thread (command link). It does NOT take m_cs:
    // the pipe callbacks (OnPipeResponse/OnPipeStatus) run on the pipe read
    // thread and lock m_cs only for their own m_statusText writes; state
    // transitions are ordered by the auth_interaction_policy guards instead.
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
        SetStatusText(L"正在识别...");
        if (m_pCredentialEvents) {
            m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        }

        m_pipeClient->StartBackgroundRead(
            [this](bool success, const std::wstring& msg) {
                OnPipeResponse(success, msg);
            },
            [this](const std::wstring& msg) {
                OnPipeStatus(msg);
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

    m_inputThreadRunning = true;
    unsigned threadId = 0;
    m_hInputThread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, InputDetectionThreadProc, this, 0, &threadId));
    if (!m_hInputThread || m_hInputThread == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"Failed to start input detection thread");
        m_inputThreadRunning = false;
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

void FaceLoginCredential::ArmFailureRetryDetection() {
    if (m_inputThreadRunning) {
        return;
    }

    // Seed the baseline HERE, not in Advise: Advise early-returns in failure
    // state, and for a re-selected failed tile the Advise-time baseline is
    // stale by a whole round. The fresh baseline ignores everything from the
    // finished round — including mouse movement while the camera filmed — so
    // only presses after this moment can request a retry.
    m_waitingStartTick = GetTickCount();
    // Same instant: snapshot keys still physically held at failure
    // presentation (e.g. the user hammering keys through the failed round).
    // The thread quarantines their auto-repeat/KEYUP ticks, so a key held
    // across the transition cannot trigger an instant re-retry — only a NEW
    // press qualifies.
    SnapshotBaselineKeys();
    StartInputDetectionThread();
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

    // Retain a private copy so GetSerialization can re-serve the credential
    // on later calls: the guard above zeroes m_password on EVERY return path
    // (security #9), so without this copy a re-polling LogonUI could never
    // serialize a second time. The copy lives only while the Ready handoff
    // is pending — ClearPackedCredentials bounds it at ReportResult, failure
    // presentation and destruction.
    ClearPackedCredentials();
    m_packedCreds = new (std::nothrow) BYTE[cbPackedCreds];
    if (!m_packedCreds) {
        CoTaskMemFree(pPackedCreds);
        return E_OUTOFMEMORY;
    }
    memcpy(m_packedCreds, pPackedCreds, cbPackedCreds);
    m_cbPackedCreds = cbPackedCreds;
    m_ulAuthPackage = ulAuthPackage;

    pcpcs->rgbSerialization = pPackedCreds;
    pcpcs->cbSerialization = cbPackedCreds;
    pcpcs->ulAuthenticationPackage = ulAuthPackage;
    pcpcs->clsidCredentialProvider = CLSID_FaceLoginProvider;

    FACELOGIN_INFO(L"PackCred SUCCESS: cbSerialization=%lu, ulAuthPackage=%lu",
                  cbPackedCreds, ulAuthPackage);
    return S_OK;
}

void FaceLoginCredential::ClearPackedCredentials() {
    if (m_packedCreds) {
        SecureZeroMemory(m_packedCreds, m_cbPackedCreds);
        delete[] m_packedCreds;
        m_packedCreds = nullptr;
    }
    m_cbPackedCreds = 0;
    m_ulAuthPackage = 0;
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

void FaceLoginCredential::SetStatusText(const std::wstring& text) {
    EnterCriticalSection(&m_cs);
    m_statusText = text;
    LeaveCriticalSection(&m_cs);
}

std::wstring FaceLoginCredential::SnapshotStatusText() {
    EnterCriticalSection(&m_cs);
    std::wstring snapshot = m_statusText;
    LeaveCriticalSection(&m_cs);
    return snapshot;
}

void FaceLoginCredential::OnPipeStatus(const std::wstring& message) {
    // A deselection may have torn the pipe down mid-flight; a late status
    // from that round must not overwrite the reset idle prompt.
    if (!facelogin::credential_provider::ShouldProcessPipeResponse(m_state)) {
        FACELOGIN_INFO(L"OnPipeStatus: dropped late status in state=%d",
                       static_cast<int>(m_state));
        return;
    }
    SetStatusText(message);
    // Use SetFieldString to update the status text in-place on the lock
    // screen, without triggering re-enumeration (which destroys the pipe).
    if (m_pCredentialEvents) {
        m_pCredentialEvents->SetFieldString(this, 1, message.c_str());
    }
}

void FaceLoginCredential::OnPipeResponse(bool success, const std::wstring& message) {
    // Same late-result guard as OnPipeStatus: a response delivered after the
    // tile was deselected (auth aborted, state reset to Waiting) is stale
    // and must not overwrite state, credentials or tile text.
    if (!facelogin::credential_provider::ShouldProcessPipeResponse(m_state)) {
        FACELOGIN_INFO(L"OnPipeResponse: dropped late result in state=%d",
                       static_cast<int>(m_state));
        return;
    }
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
            SetStatusText(L"人脸识别成功，正在解锁...");
            if (m_pCredentialEvents) {
                m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
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
    SetStatusText(statusText.empty() ? L"人脸识别失败" : statusText);

    // A terminal state ends any pending Ready handoff — the retained packed
    // credential (which carries the plaintext password) must not survive it.
    ClearPackedCredentials();

    // The reason line states WHAT failed; field 4 below owns "how to
    // retry" — strip instruction tails that would read as duplication next
    // to "请按任意键重试" (用户定稿 2026-08-29). Only the two tails whose
    // retry the hint actually serves; password/re-enroll instructions
    // (e.g. "请使用密码登录并重新录入人脸") stay — the hint does not
    // replace them.
    for (const wchar_t* tail : {L"，请重试", L"，请使用真实人脸"}) {
        const size_t tailLen = wcslen(tail);
        if (m_statusText.size() > tailLen &&
            m_statusText.compare(m_statusText.size() - tailLen, tailLen,
                                 tail) == 0) {
            m_statusText.erase(m_statusText.size() - tailLen);
            break;
        }
    }

    // Re-arm the passive watcher for the failure tile: a qualifying press
    // now requests another round (see ArmFailureRetryDetection). No-op when
    // called on the still-running watcher thread (immediate pipe connect/
    // send failure) — documented at the top of this file.
    ArmFailureRetryDetection();

    // The retry affordance is field 4 ("请按任意键重试") under the reason
    // line — a separate field because tile text ignores "\r\n". Show it
    // exactly when passive retry is live after this presentation: always
    // for Failed (those presentations never run on the watcher thread),
    // and for Error only when the running watcher is NOT the calling
    // thread — the immediate connect/send failure on the watcher thread
    // leaves nothing armed (GetThreadId identifies that caller), so its
    // "服务不可用" tile stays bare instead of promising a dead gesture.
    const bool passiveRetryLive = m_inputThreadRunning &&
        GetThreadId(m_hInputThread) != GetCurrentThreadId();
    const bool showRetryHint = failureState == State::Failed || passiveRetryLive;

    // Update the selected tile in-place. In particular, do not call
    // CredentialsChanged: that causes UnAdvise/Advise and used to restart the
    // global input watcher while the user was typing a password.
    if (m_pCredentialEvents) {
        m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        m_pCredentialEvents->SetFieldState(this, 3, CPFS_HIDDEN);
        m_pCredentialEvents->SetFieldState(
            this, 4, showRetryHint ? CPFS_DISPLAY_IN_SELECTED_TILE : CPFS_HIDDEN);
    }
    FACELOGIN_INFO(L"Terminal failure shown in-place; passive retry %s",
                   m_inputThreadRunning ? L"re-armed" : L"not armed (service down)");
}

void FaceLoginCredential::StartExplicitRetry() {
    if (!facelogin::credential_provider::IsRetryableFailure(m_state)) {
        return;
    }

    // The previous read thread has already delivered its terminal response.
    // Destroying the client joins that completed thread and guarantees the new
    // request cannot reuse a terminal pipe connection.
    m_pipeClient.reset();
    SetStatusText(L"正在识别...");
    m_state = State::Waiting;

    if (m_pCredentialEvents) {
        m_pCredentialEvents->SetFieldString(this, 1, m_statusText.c_str());
        m_pCredentialEvents->SetFieldString(this, 3, L"切换到密码登录");
        m_pCredentialEvents->SetFieldState(this, 3, CPFS_DISPLAY_IN_DESELECTED_TILE);
        m_pCredentialEvents->SetFieldState(this, 4, CPFS_HIDDEN);
    }

    // This is the single retry entry: the visible command link AND the
    // failure-tile input watcher both route here. Password input stays
    // unambiguous not because polling is disabled (it is armed while this
    // tile is selected), but because the watcher is stopped on deselect and
    // this tile has no editable field — see auth_interaction_policy.h.
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
