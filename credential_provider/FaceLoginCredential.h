#pragma once

#include <windows.h>
#include <credentialprovider.h>
#include <string>
#include <memory>
#include <vector>

#include "pipe_client.h"
#include "auth_interaction_policy.h"

// Forward declarations
class FaceLoginProvider;

// ============================================================================
// FaceLoginCredential — ICredentialProviderCredential implementation
//
// The actual credential tile shown on the lock screen. Handles:
//   1. Connecting to the face recognition service via named pipe
//   2. Waiting for face authentication result
//   3. Serializing credentials via CredPackAuthenticationBufferW
//   4. Auto-logon two-pass pattern
//
// State machine:
//   Waiting        — Initial state, trying to establish pipe connection
//   Authenticating — Pipe connected, waiting for face recognition result
//   Ready          — Credentials received, ready to serialize
//   Failed         — Auth timed out or error
// ============================================================================

class FaceLoginCredential : public ICredentialProviderCredential {
public:
    // Allow the input-detection thread to access private members
    friend unsigned __stdcall InputDetectionThreadProc(void* pParam);

    FaceLoginCredential();
    virtual ~FaceLoginCredential();

    // Called by FaceLoginProvider after creation
    void Initialize(FaceLoginProvider* pProvider);

    // Provider-level advise/unadvise (called by FaceLoginProvider::Advise/UnAdvise)
    void AdviseProvider(ICredentialProviderEvents* pEvents, UINT_PTR upAdviseContext);
    void UnadviseProvider();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ICredentialProviderCredential
    STDMETHODIMP Advise(ICredentialProviderCredentialEvents* pcpce) override;
    STDMETHODIMP_(HRESULT) UnAdvise() override;
    STDMETHODIMP SetSelected(BOOL* pbAutoLogon) override;
    STDMETHODIMP SetDeselected() override;
    STDMETHODIMP GetFieldState(DWORD dwFieldID,
                               CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
                               CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) override;
    STDMETHODIMP GetStringValue(DWORD dwFieldID, PWSTR* ppwsz) override;
    STDMETHODIMP GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp) override;
    STDMETHODIMP GetCheckboxValue(DWORD dwFieldID, BOOL* pbChecked, PWSTR* ppwszLabel) override;
    STDMETHODIMP GetSubmitButtonValue(DWORD dwFieldID, DWORD* pdwAdjacentTo) override;
    STDMETHODIMP GetComboBoxValueCount(DWORD dwFieldID, DWORD* pcItems, DWORD* pdwSelectedItem) override;
    STDMETHODIMP GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR* ppwszItem) override;
    STDMETHODIMP SetStringValue(DWORD dwFieldID, LPCWSTR pwz) override;
    STDMETHODIMP SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) override;
    STDMETHODIMP SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) override;
    STDMETHODIMP CommandLinkClicked(DWORD dwFieldID) override;
    STDMETHODIMP GetSerialization(
        CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
        PWSTR* ppwszOptionalStatusText,
        CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;
    STDMETHODIMP ReportResult(NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
                              PWSTR* ppwszOptionalStatusText,
                              CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;

private:
    using State = facelogin::credential_provider::AuthState;

    // Switch to the password credential provider (fallback)
    HRESULT SwitchToPasswordProvider();

    // Pack credentials into the serialization format
    HRESULT PackCredentials(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs);

    // Trigger re-enumeration of credentials (via CredentialsChanged)
    void TriggerReEnumeration();

    // Start the authentication pipeline (connect pipe + send AUTH_REQUEST).
    // Called from the input-detection thread (LOGON and unlock alike — the
    // provider no longer auto-triggers on cold boot).
    void StartAuth();

    // Start / stop the background input-detection thread (LOGON + unlock).
    void StartInputDetectionThread();
    void StopInputDetectionThread();

    // (Re)arm the input-detection watcher for the in-place failure tile: a
    // qualifying press (key or mouse button) then requests another face
    // round via StartExplicitRetry. Seeds its own baseline and held-key
    // snapshot (Advise refuses to in failure state); no-op while a watcher
    // is already running.
    void ArmFailureRetryDetection();

    // Snapshot of keys physically held at the baseline moment (called from
    // Advise for the first round and from ArmFailureRetryDetection for the
    // failure tile, same instant as m_waitingStartTick). Also logs every
    // held key — the one log line that proves GetAsyncKeyState works inside
    // LogonUI's secure desktop.
    void SnapshotBaselineKeys();

    // Pipe callbacks — called from background read thread
    void OnPipeResponse(bool success, const std::wstring& message);
    void OnPipeStatus(const std::wstring& message);

    // Present a terminal, retryable failure in-place on the selected tile
    // (no re-enumeration — LogonUI must not disturb password entry on other
    // tiles), then re-arm the passive watcher so a qualifying press can
    // request another round (ArmFailureRetryDetection).
    void PresentRetryableFailure(State failureState,
                                 const std::wstring& statusText);
    void StartExplicitRetry();

    LONG m_refCount = 1;
    FaceLoginProvider* m_pProvider = nullptr;
    ICredentialProviderCredentialEvents* m_pCredentialEvents = nullptr;
    ICredentialProviderEvents* m_pProviderEvents = nullptr;
    UINT_PTR m_upAdviseContext = 0;

    State m_state = State::Waiting;
    std::unique_ptr<facelogin::PipeClient> m_pipeClient;

    // Received credentials (zeroed after serialization)
    std::wstring m_upn;
    std::wstring m_domain;
    std::wstring m_username;
    std::wstring m_password;

    // Live status text pushed from service (updated from background thread)
    std::wstring m_statusText;

    // Auth timeout tracking (so we don't block LogonUI forever)
    LONGLONG m_authStartTime = 0;  // 100ns units, 0 = not yet started

    // Baseline tick recorded in Advise() (first Waiting round) or in
    // ArmFailureRetryDetection() (failure tile). A background thread polls
    // GetLastInputInfo() and triggers auth/retry when NEW input arrives
    // (keyboard or mouse). For the first round, the keypress that dismissed
    // the lock-screen wallpaper happened BEFORE our DLL was loaded, so any
    // tick <= baseline is ignored; for the failure tile the fresh baseline
    // ignores the whole finished round.
    DWORD m_waitingStartTick = 0;
    HANDLE m_hInputThread = nullptr;   // background input-detection thread
    HANDLE m_hInputStop = nullptr;     // event: signal to stop the thread
    bool m_inputThreadRunning = false;

    // Keys (any key or mouse button) still physically held when the
    // credential view appeared (first round) or when a failure was
    // presented (ArmFailureRetryDetection), snapshotted via
    // GetAsyncKeyState() right after m_waitingStartTick. Non-empty means
    // input is mid-gesture at that moment (e.g. the user holding Win+L
    // through the lock transition, or hammering keys through a failed
    // round), so the input thread must quarantine the trailing
    // auto-repeat/KEYUP ticks instead of treating them as a wave.
    // Written once before the thread starts (CreateThread establishes the
    // happens-before), so the thread reads it without a lock.
    //
    // Deliberately ANY key, not just lock-hotkey modifiers: Windows clears
    // the Win modifier's async key state at secure-desktop activation
    // (observed 2026-08-15: holding Win+L, the snapshot saw 'L' but NOT
    // LWIN), so a modifier-only fingerprint misses the primary repro — while
    // a held non-modifier keeps refreshing its state via auto-repeat and is
    // reliably visible.
    std::vector<int> m_baselineKeysHeld;

    // Synchronization
    HANDLE m_hCredsReady = nullptr;  // Set when auth result received
    CRITICAL_SECTION m_cs;
    bool m_csInitialized = false;
};
