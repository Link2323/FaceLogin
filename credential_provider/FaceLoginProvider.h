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
//
// Field layout (no tile image):
//   0: CPFT_LARGE_TEXT — "Face Login"
//   1: CPFT_SMALL_TEXT — Status message
//   2: CPFT_SUBMIT_BUTTON — Submit (hidden, auto-logon)
//   3: CPFT_COMMAND_LINK — "Switch to password login"
// ============================================================================

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
