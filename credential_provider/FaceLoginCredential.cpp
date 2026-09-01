#include "FaceLoginCredential.h"
#include "FaceLoginProvider.h"
#include "input_trigger_policy.h"
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
// Input intent is edge-based, not time-based. Raw Input supplies keyboard
// MAKE/BREAK events; physical mouse-button states are sampled because the
// lock-screen wallpaper swallows its first click. Mouse movement never enters
// the policy and therefore cannot trigger recognition. The pure policy also
// drains Win+L residue without assuming any keyboard repeat delay/rate.
//
// Secure-desktop observations (physical Windows 11 testing, 2026-08-29):
// Raw keyboard BREAK reaches this sink during wallpaper dismissal even when
// MAKE is swallowed; raw mouse DOWN/UP does not. Consequently one keyboard
// press can trigger, while the first mouse click dismisses the wallpaper and
// the second click (visible through GetAsyncKeyState on the credential view)
// triggers recognition.

static thread_local facelogin::credential_provider::InputTriggerPolicy*
    t_inputTriggerPolicy = nullptr;

static LRESULT CALLBACK RawInputSinkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RAWINPUT raw = {};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                            &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            raw.header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD& kb = raw.data.keyboard;
            // 0xFF is the documented fake-key marker and must be ignored.
            if (t_inputTriggerPolicy && kb.VKey != 0 && kb.VKey != 0xFF) {
                t_inputTriggerPolicy->ObserveKey(
                    kb.VKey, (kb.Flags & RI_KEY_BREAK) == 0);
            }
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
    using facelogin::credential_provider::InputTriggerPolicy;
    using facelogin::credential_provider::InputTriggerKind;

    constexpr DWORD kPollIntervalMs = 10;
    constexpr DWORD kMouseSelectionSettleMs = 100;
    constexpr int kFirstVk = 0x01;
    constexpr int kLastVk = 0xFE;
    constexpr int kMouseButtons[] = {
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
    };

    auto isMouseButton = [&](int vk) noexcept {
        for (int button : kMouseButtons) {
            if (vk == button) return true;
        }
        return false;
    };

    InputTriggerPolicy policy(pCred->m_inputDetectionRound);
    const bool failureRetryRound =
        pCred->m_inputDetectionRound ==
        facelogin::credential_provider::InputDetectionRound::FailureRetry;
    t_inputTriggerPolicy = &policy;

    RawInputSink rawSink;
    const bool rawKeyboardAvailable = rawSink.Register();

    std::array<bool, 256> sampledKeyDown{};
    std::array<bool, 256> sampledMouseDown{};

    // Seed inputs already held at watcher start. Mouse buttons are always
    // conservative because the first click belongs to the wallpaper. Keyboard
    // keys are seeded only for a visible failure tile; on the initial lock
    // round an already-started ordinary press must still trigger on BREAK,
    // while the policy independently drains the ambiguous L/Win tail.
    for (int vk = kFirstVk; vk <= kLastVk; ++vk) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (isMouseButton(vk)) {
            sampledMouseDown[vk] = down;
            if (down) policy.SeedMouseButtonDown(static_cast<std::uint16_t>(vk));
        } else if (!rawKeyboardAvailable) {
            sampledKeyDown[vk] = down;
            if (down && failureRetryRound) {
                policy.SeedKeyDown(static_cast<std::uint16_t>(vk));
            }
        } else if (down && failureRetryRound) {
            policy.SeedKeyDown(static_cast<std::uint16_t>(vk));
        }
    }

    bool shouldTrigger = false;
    InputTriggerKind triggerKind = InputTriggerKind::None;
    while (WaitForSingleObject(pCred->m_hInputStop, 0) != WAIT_OBJECT_0) {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&msg);
        }

        // Mouse movement is deliberately absent. Only physical button state
        // transitions reach the policy, so brushing the mouse cannot open the
        // camera. The wallpaper consumes the first click; the second is seen
        // here after the credential view becomes active.
        for (int vk : kMouseButtons) {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (down != sampledMouseDown[vk]) {
                sampledMouseDown[vk] = down;
                policy.ObserveMouseButton(static_cast<std::uint16_t>(vk), down);
            }
        }

        // If Raw Input registration fails, retain a credential-view-only
        // fallback using physical keyboard edges. It cannot see the wallpaper
        // keypress, but it still avoids movement and held-key false triggers.
        if (!rawKeyboardAvailable) {
            for (int vk = kFirstVk; vk <= kLastVk; ++vk) {
                if (isMouseButton(vk)) continue;
                const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down != sampledKeyDown[vk]) {
                    sampledKeyDown[vk] = down;
                    policy.ObserveKey(static_cast<std::uint16_t>(vk), down);
                }
            }
        }

        if (policy.triggered()) {
            shouldTrigger = true;
            triggerKind = policy.triggerKind();
            break;
        }

        DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &pCred->m_hInputStop, kPollIntervalMs, QS_ALLINPUT,
            MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
    }

    t_inputTriggerPolicy = nullptr;
    rawSink.Unregister();  // idempotent no-op if registration failed

    // A mouse click can target another sign-in option. LogonUI dispatches the
    // tile switch at the end of that click and calls SetDeselected, which
    // signals m_hInputStop. Give that UI transition one short cancellable
    // window before opening the authentication pipe. Keyboard presses retain
    // their immediate path.
    if (shouldTrigger && triggerKind == InputTriggerKind::MouseButton &&
        WaitForSingleObject(pCred->m_hInputStop, kMouseSelectionSettleMs) ==
            WAIT_OBJECT_0) {
        shouldTrigger = false;
    }

    // Deselect/UnAdvise wins a race with the qualifying edge: once the stop
    // event is signaled this watcher must not open a new pipe while its owner
    // is joining the thread and tearing the selected tile down.
    if (shouldTrigger &&
        WaitForSingleObject(pCred->m_hInputStop, 0) != WAIT_OBJECT_0) {
        if (facelogin::credential_provider::IsRetryableFailure(pCred->m_state)) {
            FACELOGIN_INFO(L"[InputThread] Qualifying press on failure tile — "
                           L"requesting explicit retry");
            pCred->StartExplicitRetry();
        } else {
            pCred->StartAuth();
        }
    }

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
    // the selected one. ArmFailureRetryDetection selects retry-round edge
    // semantics itself.
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
    // Advise still selects initial-lock edge semantics; SetSelected starts the
    // watcher only when the user actually lands on this tile.
    // (CredUI/PLAP never reach this point — SetUsageScenario already
    // returned E_NOTIMPL for them.)
    if (!facelogin::credential_provider::ShouldStartInputDetection(m_state)) {
        FACELOGIN_INFO(L"Advise: state does not permit passive input detection");
        return S_OK;
    }
    m_inputDetectionRound =
        facelogin::credential_provider::InputDetectionRound::InitialLock;

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
    // re-arms the failure watcher with fresh-edge semantics, so the tail of
    // the key/click that performed the re-selection cannot trigger a retry.
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

    // A naturally completed watcher clears m_inputThreadRunning itself, but
    // its signaled thread handle and stop event are still owner resources.
    // Reap them before starting another round instead of overwriting/leaking
    // the old handles.
    if (m_hInputThread || m_hInputStop) {
        StopInputDetectionThread();
    }

    m_hInputStop = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_hInputStop) {
        FACELOGIN_ERROR(L"Failed to create input stop event");
        return;
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
    if (!m_inputThreadRunning && !m_hInputThread && !m_hInputStop) {
        return;
    }

    const bool wasRunning = m_inputThreadRunning;
    if (wasRunning) {
        FACELOGIN_INFO(L"Stopping input detection thread...");
    }

    // Signal only a live watcher. A naturally completed watcher merely needs
    // its already-signaled thread handle and event reaped.
    if (wasRunning && m_hInputStop) {
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
    if (wasRunning) {
        FACELOGIN_INFO(L"Input detection thread stopped");
    }
}

void FaceLoginCredential::ArmFailureRetryDetection() {
    if (m_inputThreadRunning) {
        return;
    }

    // Failure tiles are already fully visible: require a fresh MAKE/button
    // down and seed every physically held input before the watcher loop. This
    // rejects auto-repeat and release tails without a timing window.
    m_inputDetectionRound =
        facelogin::credential_provider::InputDetectionRound::FailureRetry;
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
    // A click on the password command link is also visible to the global
    // mouse watcher. Cancel that watcher (or its mouse-settle window) before
    // re-enumerating providers, and tear down a round that may already have
    // started through an earlier input race.
    StopInputDetectionThread();
    if (facelogin::credential_provider::ShouldAbortAuthOnDeselect(m_state)) {
        m_pipeClient.reset();
    }

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
