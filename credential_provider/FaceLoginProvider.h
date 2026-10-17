#pragma once

#include <windows.h>
#include <credentialprovider.h>

// Forward declarations
class FaceLoginCredential;

// ============================================================================
// FaceLoginProvider — ICredentialProvider implementation
//
// Enumerates credential tiles. For face login, we always provide exactly
// one credential tile that supports auto-logon once credentials are ready.
// Recognition is always triggered by user input (keyboard/mouse), for both
// LOGON (boot / switch user) and UNLOCK — see FaceLoginCredential::Advise.
// ============================================================================

// Credential tile field indices, shared by the descriptor table below and
// every GetFieldState/GetStringValue/SetField* call in FaceLoginCredential.
// Field 0 is pinned by the failure-tile design (keep first, CPFT_LARGE_TEXT);
// field 3 doubles as the password-switch command link and carries retry-tile
// visibility state. Indices are the wire contract with LogonUI — inserting
// or reordering fields changes what the tile renders, not just our code.
enum CredentialField {
    kFieldTitle      = 0,  // CPFT_LARGE_TEXT — "人脸登录"
    kFieldStatus     = 1,  // CPFT_SMALL_TEXT — status / failure reason
    kFieldSubmit     = 2,  // CPFT_SUBMIT_BUTTON — hidden, auto-logon path
    kFieldSwitchLink = 3,  // CPFT_COMMAND_LINK — "切换到密码登录"
    kFieldRetryHint  = 4,  // CPFT_SMALL_TEXT — "请按任意键重试" hint line
};

class FaceLoginProvider : public ICredentialProvider {
public:
    FaceLoginProvider();
    virtual ~FaceLoginProvider();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ICredentialProvider
    STDMETHODIMP SetUsageScenario(
        CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
        DWORD dwFlags) override;

    STDMETHODIMP SetSerialization(
        const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) override;

    STDMETHODIMP Advise(
        ICredentialProviderEvents* pcpe,
        UINT_PTR upAdviseContext) override;

    STDMETHODIMP UnAdvise() override;

    STDMETHODIMP GetFieldDescriptorCount(
        DWORD* pdwCount) override;

    STDMETHODIMP GetFieldDescriptorAt(
        DWORD dwIndex,
        CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd) override;

    STDMETHODIMP GetCredentialCount(
        DWORD* pdwCount,
        DWORD* pdwDefault,
        BOOL* pbAutoLogonWithDefault) override;

    STDMETHODIMP GetCredentialAt(
        DWORD dwIndex,
        ICredentialProviderCredential** ppcpc) override;

private:
    LONG m_refCount = 1;
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR m_rgFieldDescriptors[5];
    ICredentialProviderEvents* m_pEvents = nullptr;
    UINT_PTR m_upAdviseContext = 0;

    // Our credential object (one instance)
    FaceLoginCredential* m_pCredential = nullptr;
};
