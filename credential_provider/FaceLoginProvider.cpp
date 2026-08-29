#include "FaceLoginProvider.h"
#include "FaceLoginCredential.h"
#include "../common/logger.h"
#include "../common/registry_util.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <fstream>
#include <cstdint>

#pragma comment(lib, "credui.lib")
#pragma comment(lib, "advapi32.lib")

// External GUID defined in resource.h / initguid
extern const GUID CLSID_FaceLoginProvider;

FaceLoginProvider::FaceLoginProvider() {
    FACELOGIN_INFO(L"FaceLoginProvider created");

    // Define fields for our credential tile (no tile image — text only)

    // Field 0: Large text ("人脸登录")
    m_rgFieldDescriptors[0].dwFieldID = 0;
    m_rgFieldDescriptors[0].cpft = CPFT_LARGE_TEXT;
    m_rgFieldDescriptors[0].pszLabel = const_cast<LPWSTR>(L"人脸登录");
    m_rgFieldDescriptors[0].guidFieldType = GUID_NULL;

    // Field 1: Small text (status message)
    m_rgFieldDescriptors[1].dwFieldID = 1;
    m_rgFieldDescriptors[1].cpft = CPFT_SMALL_TEXT;
    m_rgFieldDescriptors[1].pszLabel = const_cast<LPWSTR>(L"状态");
    m_rgFieldDescriptors[1].guidFieldType = GUID_NULL;

    // Field 2: Submit button (hidden, auto-logon handles submission)
    m_rgFieldDescriptors[2].dwFieldID = 2;
    m_rgFieldDescriptors[2].cpft = CPFT_SUBMIT_BUTTON;
    m_rgFieldDescriptors[2].pszLabel = const_cast<LPWSTR>(L"提交");
    m_rgFieldDescriptors[2].guidFieldType = GUID_NULL;

    // Field 3: Command link (switch to password)
    m_rgFieldDescriptors[3].dwFieldID = 3;
    m_rgFieldDescriptors[3].cpft = CPFT_COMMAND_LINK;
    m_rgFieldDescriptors[3].pszLabel = const_cast<LPWSTR>(L"切换到密码登录");
    m_rgFieldDescriptors[3].guidFieldType = GUID_NULL;

    // Field 4: Small text — the "请按任意键重试" hint line on failure tiles.
    // Tile text fields do not render "\r\n" as a line break (装机实测
    // 2026-08-29), so the two-line layout (reason / hint) needs its own
    // field instead of an embedded newline in the status text.
    m_rgFieldDescriptors[4].dwFieldID = 4;
    m_rgFieldDescriptors[4].cpft = CPFT_SMALL_TEXT;
    m_rgFieldDescriptors[4].pszLabel = const_cast<LPWSTR>(L"重试提示");
    m_rgFieldDescriptors[4].guidFieldType = GUID_NULL;
}

FaceLoginProvider::~FaceLoginProvider() {
    FACELOGIN_INFO(L"FaceLoginProvider destroyed");
    if (m_pCredential) {
        m_pCredential->Release();
        m_pCredential = nullptr;
    }
}

// ============================================================================
// Helper: check if any enrolled users exist
// ============================================================================

static DWORD ReadUserCountFromDatabase() {
    // Path precedence: the registry DataPath (written by the installer as the
    // install directory — where users.dat actually lives in production) wins;
    // %PROGRAMDATA%\FaceLogin is only the fallback when it is unset.
    wchar_t programData[MAX_PATH];
    std::wstring dataDir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
        dataDir = std::wstring(programData) + L"\\FaceLogin";
    } else {
        dataDir = L"C:\\ProgramData\\FaceLogin";
    }
    std::wstring regPath = ReadRegString(REGVAL_DATA_PATH, L"");
    if (!regPath.empty()) {
        dataDir = regPath;
    }
    std::wstring filePath = dataDir + L"\\data\\users.dat";

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return 0;  // No database → no users
    }

    // Read header: magic (4), version (4), count (4)
    uint32_t magic = 0, version = 0, count = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (magic != 0x474F4C46 || version != 4) {  // "FLOG"
        return 0;  // Invalid database → treat as no users
    }

    return count;
}

// ============================================================================
// IUnknown
// ============================================================================

STDMETHODIMP FaceLoginProvider::QueryInterface(REFIID riid, void** ppv) {
    *ppv = nullptr;
    HRESULT hr = E_NOINTERFACE;

    if (riid == IID_IUnknown || riid == IID_ICredentialProvider) {
        *ppv = static_cast<ICredentialProvider*>(this);
        AddRef();
        hr = S_OK;
    }

    return hr;
}

STDMETHODIMP_(ULONG) FaceLoginProvider::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) FaceLoginProvider::Release() {
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// ICredentialProvider
// ============================================================================

STDMETHODIMP FaceLoginProvider::SetUsageScenario(
    CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags) {
    FACELOGIN_INFO(L"SetUsageScenario: cpus=%d, flags=0x%08X", cpus, dwFlags);

    // CPUS_CHANGE_PASSWORD: we don't support changing passwords via face
    // recognition. Let the built-in password provider handle this.
    if (cpus == CPUS_CHANGE_PASSWORD) {
        FACELOGIN_INFO(L"SetUsageScenario: CPUS_CHANGE_PASSWORD — delegating to password provider");
        return E_NOTIMPL;
    }

    // CPUS_CREDUI / CPUS_PLAP: Windows Security dialog popped up from within
    // an active user session (e.g. PIN change, fingerprint enrollment, Edge
    // password viewing, etc.).  These dialogs are multi-step workflows that
    // don't work reliably with our face recognition flow — some dismiss
    // prematurely, others don't accept the credential format.  Always delegate
    // to the built-in password/pin providers.
    if (cpus == CPUS_CREDUI || cpus == CPUS_PLAP) {
        FACELOGIN_INFO(L"SetUsageScenario: CPUS_CREDUI/CPUS_PLAP — delegating to password provider");
        return E_NOTIMPL;
    }

    // ── LOGON / UNLOCK → trigger policy ─────────────────────────────
    // Both scenarios wait for user input (keyboard/mouse) before starting
    // recognition — FaceLoginCredential::Advise runs the same input-detection
    // thread everywhere.  Previously the CP distinguished "cold boot" (auto-
    // trigger StartAuth immediately) from unlock (wait for keypress): at boot
    // the camera started recognizing before the user was at the machine, and
    // because every failed attempt triggered re-enumeration with coldBoot
    // still true, an empty scene looped recognition forever.  Removed 2026-08;
    // the ServiceStartUptime/UserLoggedIn registry machinery is gone too
    // (UserLoggedIn remains for the service's model-preload decision).

    // Check if the Disabled registry flag is set
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD disabled = 0, size = sizeof(disabled);
        RegQueryValueExW(hKey, L"Disabled", nullptr, nullptr,
                        reinterpret_cast<LPBYTE>(&disabled), &size);
        RegCloseKey(hKey);
        if (disabled) {
            FACELOGIN_INFO(L"Provider is disabled via registry");
            return E_NOTIMPL;  // This will cause LogonUI to skip this provider
        }
    }

    // Check if any users have been enrolled. If not, hide the face login
    // tile entirely — no point showing it to a first-time user.
    DWORD userCount = ReadUserCountFromDatabase();
    FACELOGIN_INFO(L"User count from database: %lu", userCount);
    if (userCount == 0) {
        FACELOGIN_INFO(L"No enrolled users — hiding face login tile");
        return E_NOTIMPL;
    }

    // Create our credential
    m_pCredential = new FaceLoginCredential();
    if (!m_pCredential) {
        return E_OUTOFMEMORY;
    }
    m_pCredential->Initialize(this);

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::SetSerialization(
    const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) {
    // We don't handle incoming credential serialization.
    // This is called for things like RDP pre-fill or CredUI callbacks.
    UNREFERENCED_PARAMETER(pcpcs);
    return S_OK;
}

STDMETHODIMP FaceLoginProvider::Advise(
    ICredentialProviderEvents* pcpe, UINT_PTR upAdviseContext) {
    FACELOGIN_INFO(L"Advise called");

    if (m_pEvents) {
        m_pEvents->Release();
    }

    m_pEvents = pcpe;
    m_upAdviseContext = upAdviseContext;

    if (m_pEvents) {
        m_pEvents->AddRef();
    }

    if (m_pCredential) {
        m_pCredential->AdviseProvider(m_pEvents, upAdviseContext);
    }

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::UnAdvise() {
    FACELOGIN_INFO(L"UnAdvise called");

    if (m_pEvents) {
        m_pEvents->Release();
        m_pEvents = nullptr;
    }

    if (m_pCredential) {
        m_pCredential->UnadviseProvider();
    }

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::GetFieldDescriptorCount(DWORD* pdwCount) {
    *pdwCount = ARRAYSIZE(m_rgFieldDescriptors);
    return S_OK;
}

STDMETHODIMP FaceLoginProvider::GetFieldDescriptorAt(
    DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd) {

    if (dwIndex >= ARRAYSIZE(m_rgFieldDescriptors)) {
        return E_INVALIDARG;
    }

    // Allocate and copy the field descriptor (replacing FieldDescriptorCoAllocCopy)
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR* pcpfd =
        static_cast<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*>(
            CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR)));
    if (!pcpfd) {
        return E_OUTOFMEMORY;
    }
    *pcpfd = m_rgFieldDescriptors[dwIndex];
    pcpfd->pszLabel = nullptr;
    HRESULT hr = ::SHStrDupW(m_rgFieldDescriptors[dwIndex].pszLabel, &pcpfd->pszLabel);
    if (FAILED(hr)) {
        CoTaskMemFree(pcpfd);
        return hr;
    }
    *ppcpfd = pcpfd;

    return hr;
}

STDMETHODIMP FaceLoginProvider::GetCredentialCount(
    DWORD* pdwCount, DWORD* pdwDefault, BOOL* pbAutoLogonWithDefault) {

    // We always provide exactly one credential
    *pdwCount = 1;
    *pdwDefault = 0;

    // No default auto-logon: recognition is always triggered by user input
    // (the input-detection thread started in Advise), for both LOGON and
    // UNLOCK.  SetSelected() flips auto-logon to TRUE once credentials are
    // Ready so LogonUI calls GetSerialization to pack them.
    *pbAutoLogonWithDefault = FALSE;
    FACELOGIN_INFO(L"GetCredentialCount: count=%d, default=%d, autoLogon=%d",
                  *pdwCount, *pdwDefault, *pbAutoLogonWithDefault);

    return S_OK;
}

STDMETHODIMP FaceLoginProvider::GetCredentialAt(
    DWORD dwIndex, ICredentialProviderCredential** ppcpc) {

    if (dwIndex != 0 || !m_pCredential) {
        return E_INVALIDARG;
    }

    return m_pCredential->QueryInterface(IID_ICredentialProviderCredential,
                                         reinterpret_cast<void**>(ppcpc));
}
