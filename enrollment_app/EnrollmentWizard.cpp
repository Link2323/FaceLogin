#include "EnrollmentWizard.h"
#include "../common/logger.h"
#include "../common/dpapi_util.h"
#include "../common/ipc_protocol.h"
#include "../common/registry_util.h"
#include "../common/config_util.h"
#include "../common/image_utils.h"
#include <comdef.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wincred.h>
#include <sddl.h>
#include <lmaccess.h>
#include <lmapibuf.h>
#include <lmerr.h>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <chrono>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace facelogin {

// Helper: convert UTF-8 string to wide string (for config camera_device)
static std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

// ---------------------------------------------------------------------------
// Session identity helpers
//
// The authoritative UPN source is GetUserNameExW(NameUserPrincipal): it
// returns the email for Microsoft accounts and fails with ERROR_NONE_MAPPED
// (1332) for local accounts. On a subset of machines, however, the whole UPN
// family of name formats fails with 1332 even for a live MSA session
// (observed on Windows 11 with a local SAM account linked to a Microsoft
// account: only NameFullyQualifiedDN / NameSamCompatible succeed;
// UserPrincipal/Display/Canonical/CanonicalEx/SPN/DnsDomain all return 1332).
// In that case the UPN is empty and every MSA/local decision collapses to
// "local" — the UI then says "Windows 密码" while the password that actually
// authenticates is the Microsoft account password (the user-visible symptom
// we are fixing).
//
// Fallback: scan the process token's ENABLED groups for the MSA identity.
// LSA injects a group SID under the MicrosoftAccount\S-1-11-96-... authority
// into the token of any session that logged on via a linked Microsoft
// account, and LookupAccountSidW resolves its name to the email (e.g.
// "MicrosoftAccount\shi2jun2cheng2@outlook.com"). This is bound to the
// CURRENT session token, so unlike the machine-wide IdentityStore\LogonCache
// registry caches it can never carry another user's leftover MSA email — the
// exact misattribution that produced bug1's perpetual false "账号身份已变更"
// prompt (docs/todo.md bug1). IdentityStore\LogonCache\Name2Sid and
// IdentityCRL\StoredIdentities must NOT be re-added as fallbacks.
// ---------------------------------------------------------------------------

// Primary UPN query. Returns empty on failure (1332 or no secur32).
static std::wstring GetSessionUpn() {
    std::wstring upn;
    HMODULE hSecur32 = LoadLibraryW(L"secur32.dll");
    if (!hSecur32) return upn;
    typedef BOOLEAN (WINAPI *PFN_GetUserNameExW)(int, LPWSTR, PULONG);
    auto pfn = reinterpret_cast<PFN_GetUserNameExW>(
        GetProcAddress(hSecur32, "GetUserNameExW"));
    if (pfn) {
        ULONG upnSize = 256;
        std::vector<wchar_t> upnBuf(upnSize);
        if (pfn(8 /* NameUserPrincipal */, upnBuf.data(), &upnSize)) {
            upn = upnBuf.data();
        }
    }
    FreeLibrary(hSecur32);
    return upn;
}

// Fallback used only when GetSessionUpn() returned empty: scans the current
// process token's ENABLED groups for the MSA identity LSA injects and returns
// the email portion. Empty when the session is genuinely local (no MSA group)
// or the SID cannot be resolved.
//
// The S-1-11-96-... prefix (identifier authority 11, first sub-authority 96)
// is the MicrosoftAccount namespace — used as a cheap filter before the
// authoritative LookupAccountSidW call. We only consider enabled groups
// (SE_GROUP_ENABLED) and skip deny-only / disabled groups, since a disabled
// MSA group does not represent the active logon identity.
static std::wstring ResolveSessionMsaUpnFromToken() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    std::wstring result;
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenGroups, nullptr, 0, &sz);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetTokenInformation(hToken, TokenGroups, buf.data(), sz, &sz)) {
            auto* tg = reinterpret_cast<TOKEN_GROUPS*>(buf.data());
            for (DWORD i = 0; i < tg->GroupCount; ++i) {
                // Only enabled groups represent the active identity; skip
                // SE_GROUP_USE_FOR_DENY_ONLY and disabled groups.
                if (!(tg->Groups[i].Attributes & SE_GROUP_ENABLED)) {
                    continue;
                }
                PSID sid = tg->Groups[i].Sid;
                if (!sid) continue;

                // Cheap filter on the string form: S-1-11-96-... is the
                // MicrosoftAccount namespace.
                LPWSTR sidStr = nullptr;
                if (!ConvertSidToStringSidW(sid, &sidStr)) continue;
                std::wstring sidStrW = sidStr;
                LocalFree(sidStr);
                if (sidStrW.rfind(L"S-1-11-96-", 0) != 0) continue;

                // Authoritative check: domain must be "MicrosoftAccount" and
                // the name must contain '@' (the email form).
                wchar_t name[256] = {};
                wchar_t domain[256] = {};
                DWORD nameLen = ARRAYSIZE(name);
                DWORD domainLen = ARRAYSIZE(domain);
                SID_NAME_USE sidType;
                if (LookupAccountSidW(nullptr, sid, name, &nameLen,
                                      domain, &domainLen, &sidType)) {
                    std::wstring nameW = name;
                    std::wstring domainW = domain;
                    if (domainW == L"MicrosoftAccount" &&
                        nameW.find(L'@') != std::wstring::npos) {
                        FACELOGIN_INFO(L"ResolveSessionMsaUpnFromToken: MSA group SID hit %s -> %s\\%s",
                                       sidStrW.c_str(), domainW.c_str(), nameW.c_str());
                        result = nameW;
                        break;
                    }
                }
            }
        }
    }
    CloseHandle(hToken);
    return result;
}

// Unified entry point for ALL MSA/local decisions. Try the authoritative
// GetUserNameExW query first; on 1332/empty fall back to the token MSA group.
static std::wstring ResolveSessionUpn() {
    std::wstring upn = GetSessionUpn();
    if (!upn.empty() && upn.find(L'@') != std::wstring::npos) {
        return upn;
    }
    return ResolveSessionMsaUpnFromToken();
}

// MSA test used everywhere: UPN non-empty and contains '@'.
static bool IsMsaUpn(const std::wstring& upn) {
    return !upn.empty() && upn.find(L'@') != std::wstring::npos;
}

EnrollmentWizard::EnrollmentWizard() {
    std::wstring regData = ReadRegString(REGVAL_DATA_PATH, L"");
    if (!regData.empty()) {
        m_dataDir = regData;
    } else {
        wchar_t programData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData))) {
            m_dataDir = std::wstring(programData) + L"\\FaceLogin";
        } else {
            m_dataDir = L"C:\\ProgramData\\FaceLogin";
        }
    }
    CreateDirectoryW(m_dataDir.c_str(), nullptr);

    std::wstring logPath = m_dataDir + L"\\log\\enrollment.log";
    Logger::Instance().SetLogFile(logPath);
    Logger::Instance().SetMinLevel(LogLevel::Info);
    Logger::Instance().SetEnableDebugOutput(true);

    FACELOGIN_INFO(L"=== Enrollment Wizard started ===");

    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&m_wicFactory));

    // --- Gather user identity ---
    {
        wchar_t username[256] = {};
        DWORD size = ARRAYSIZE(username);
        if (!GetUserNameW(username, &size)) {
            DWORD err = GetLastError();
            FACELOGIN_ERROR(L"GetUserNameW FAILED: err=%lu", err);
            username[0] = L'\0';
        }
        m_username = username;
    }

    // Get UPN (UserPrincipalName) — the ONLY trusted source of the current
    // session's identity. GetUserNameExW(NameUserPrincipal) returns the email
    // UPN for Microsoft accounts and fails with ERROR_NONE_MAPPED for local
    // accounts, so m_upn stays empty for local users.
    //
    // NOTE: machine-wide registry fallbacks (IdentityCRL\StoredIdentities,
    // IdentityStore\LogonCache\Name2Sid) used to live here and adopted ANY
    // cached MSA email without verifying it belongs to the current user. On
    // machines with a leftover MSA identity (a previous user profile, or an
    // MSA later converted to local) that mislabeled local accounts as MSA and
    // poisoned the stored record — producing a perpetual false
    // "账号身份已变更" prompt (docs/todo.md bug1). Never re-add an
    // unattributed cache lookup.
    //
    // ResolveSessionUpn() additionally falls back to the process-token MSA
    // group when GetUserNameExW fails (see comment at its definition) — this
    // machine exhibits exactly that 1332 failure for the whole UPN family.
    m_upn = ResolveSessionUpn();

    // Determine account type: MSA accounts have a UPN containing '@'
    m_accountType = "local";
    if (IsMsaUpn(m_upn)) {
        m_accountType = "msa";
    }

    // Get SID via LookupAccountNameW
    {
        DWORD sidSize = 0, domainSize = 0;
        SID_NAME_USE sidType;
        std::wstring lookupName = m_upn.empty() ? m_username : m_upn;

        LookupAccountNameW(nullptr, lookupName.c_str(),
                           nullptr, &sidSize, nullptr, &domainSize, &sidType);
        if (sidSize > 0) {
            std::vector<BYTE> sidBuf(sidSize);
            std::vector<wchar_t> domainBuf(domainSize > 0 ? domainSize : 1);
            if (LookupAccountNameW(nullptr, lookupName.c_str(),
                                   sidBuf.data(), &sidSize,
                                   domainBuf.data(), &domainSize, &sidType)) {
                LPWSTR sidStr = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf.data()), &sidStr)) {
                    m_sid = sidStr;
                    LocalFree(sidStr);
                }
            } else {
                DWORD err = GetLastError();
                FACELOGIN_WARN(L"LookupAccountNameW(UPN) FAILED: err=%lu", err);
            }
        }

        if (m_sid.empty()) {
            DWORD sidSize2 = 0, domainSize2 = 0;
            SID_NAME_USE sidType2;
            LookupAccountNameW(nullptr, m_username.c_str(),
                               nullptr, &sidSize2, nullptr, &domainSize2, &sidType2);
            if (sidSize2 > 0) {
                std::vector<BYTE> sidBuf2(sidSize2);
                std::vector<wchar_t> domainBuf2(domainSize2 > 0 ? domainSize2 : 1);
                if (LookupAccountNameW(nullptr, m_username.c_str(),
                                       sidBuf2.data(), &sidSize2,
                                       domainBuf2.data(), &domainSize2, &sidType2)) {
                    LPWSTR sidStr = nullptr;
                    if (ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBuf2.data()), &sidStr)) {
                        m_sid = sidStr;
                        LocalFree(sidStr);
                    }
                } else {
                    DWORD err = GetLastError();
                    FACELOGIN_WARN(L"LookupAccountNameW(SAM) FAILED: err=%lu", err);
                }
            }
        }
    }

    // Identity summary — referenced by the MSA-detection verification steps.
    FACELOGIN_INFO(L"Session identity: username=%s upn=%s sid=%s accountType=%hs",
                   m_username.c_str(),
                   m_upn.empty() ? L"<empty>" : m_upn.c_str(),
                   m_sid.empty() ? L"<empty>" : m_sid.c_str(),
                   m_accountType.c_str());

    m_webcam     = std::make_unique<WebcamCapture>();
    m_store.SetDataDir(m_dataDir);

    m_config = LoadConfig(m_dataDir);
    m_livenessMethod = m_config.liveness_method;
    m_antiSpoofThreshold = m_config.anti_spoof_threshold;
}

EnrollmentWizard::~EnrollmentWizard() {
    StopPreview();
    m_capturing = false;
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_wicFactory) m_wicFactory->Release();
}

// ============================================================================
// Preview Control
// ============================================================================

bool EnrollmentWizard::StartPreview() {
    if (m_previewRunning) return true;

    if (!m_webcam->Initialize(1280, 720, Utf8ToWstr(m_config.camera_device))) {
        FACELOGIN_ERROR(L"Failed to initialize webcam%s",
                        m_config.camera_device.empty() ? L"" : L" (configured device)");
        return false;
    }

    std::wstring modelsDir = m_dataDir + L"\\models";

    // Load SCRFD ONNX detector (gnkps variant — provides the 5 alignment
    // keypoints directly; no separate landmark model anymore).
    // 10g tier: ~3.4x faster than 34g at -1% WIDER Face (96.17→95.19).
    m_onnxDetector = std::make_unique<OnnxDetector>();
    std::wstring detPath = modelsDir + L"\\det_10g_gnkps.onnx";
    if (!m_onnxDetector->Initialize(detPath)) {
        FACELOGIN_ERROR(L"SCRFD detector failed to load — enrollment unavailable");
        m_webcam->Shutdown();
        return false;
    }

    // Load InsightFace ONNX recognizer (the only recognizer).
    m_onnxRecognizer = std::make_unique<OnnxRecognizer>();
    std::wstring onnxPath = modelsDir + L"\\w600k_r50.onnx";
    if (!m_onnxRecognizer->Initialize(onnxPath)) {
        FACELOGIN_ERROR(L"ONNX recognizer failed to load — enrollment unavailable");
        m_webcam->Shutdown();
        return false;
    }
    m_onnxRecognizer->SetLowLightEnhance(m_config.low_light_enhance);

    // Anti-spoof is mandatory for enrollment.  Allowing capture without it
    // would let a photograph become the trusted template for later logons.
    m_antiSpoof = std::make_unique<OnnxAntiSpoof>();
    std::wstring miniFasV2Path = modelsDir + L"\\MiniFASNetV2.onnx";
    std::wstring miniFasV1SePath = modelsDir + L"\\MiniFASNetV1SE.onnx";
    if (!m_antiSpoof->Initialize(miniFasV2Path, miniFasV1SePath)) {
        FACELOGIN_ERROR(L"Dual MiniFAS PAD unavailable — enrollment refused");
        m_antiSpoof.reset();
        m_webcam->Shutdown();
        return false;
    }
    // dlib recognizer/detector were removed — pure ONNX. recognition_model
    // and detector config values are ignored.

    // Validate liveness method — no identity-only fallback is permitted.
    if (m_livenessMethod == LivenessMethod::Blink) {
        FACELOGIN_WARN(L"liveness_method=blink no longer supported (dlib 68-point removed) — using anti-spoof");
        m_livenessMethod = LivenessMethod::AntiSpoof;
    }
    if (m_livenessMethod != LivenessMethod::AntiSpoof ||
        !m_antiSpoof || !m_antiSpoof->IsInitialized()) {
        FACELOGIN_ERROR(L"No supported liveness method available — enrollment refused");
        m_webcam->Shutdown();
        return false;
    }

    FACELOGIN_INFO(L"Liveness method: antispoof (mandatory) | Preview started: 1280x720");

    m_previewRunning = true;
    m_frameRunning = true;

    // Single background thread: GrabFrame → JPEG encode → detect → update caches.
    // The UI thread stays completely free; JS polls the caches via GetLatest*().
    m_frameThread = std::thread([this]() {
        while (m_frameRunning) {
            dlib::matrix<dlib::rgb_pixel> frame;
            if (!m_webcam->GrabFrame(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            RotateFrame(frame, m_config.camera_rotation);

            std::string b64 = EncodeJPEGBase64(frame);
            // Preview overlay: detect the face with SCRFD and show its box.
            std::string faceJson = "[]";
            if (m_onnxDetector) {
                auto det = m_onnxDetector->DetectLargestFace(frame);
                if (det) {
                    std::vector<facelogin::FaceWithKps> faces;
                    FaceWithKps fwl;
                    fwl.rect = dlib::rectangle(static_cast<long>(det->x1),
                                               static_cast<long>(det->y1),
                                               static_cast<long>(det->x2),
                                               static_cast<long>(det->y2));
                    for (int k = 0; k < 10; k++) fwl.kps[k] = det->kps[k];
                    faces.push_back(std::move(fwl));
                    faceJson = FacesToJson(faces);
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_frameCacheMutex);
                m_latestFrameB64  = std::move(b64);
                m_latestFacesJson = std::move(faceJson);
                m_latestFrame     = frame;
                ++m_latestFrameSequence;
            }
        }
    });

    return true;
}

void EnrollmentWizard::StopPreview() {
    m_previewRunning = false;
    m_frameRunning = false;
    m_capturing = false;

    // Join background threads before shutting down camera
    if (m_captureThread.joinable())
        m_captureThread.join();
    if (m_frameThread.joinable())
        m_frameThread.join();

    if (m_webcam)
        m_webcam->Shutdown();
}

// ============================================================================
// Per-frame Processing (called from timer callback)
// ============================================================================

std::string EnrollmentWizard::GetLatestFrameBase64() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    return m_latestFrameB64;
}

std::string EnrollmentWizard::GetLatestFacesJson() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    return m_latestFacesJson;
}

// Atomically return BOTH the current frame (JPEG base64) and its face overlay
// JSON. The frame thread updates them together under the same lock, so reading
// them in one call guarantees they belong to the SAME frame. The frontend uses
// this to draw the background and overlay from matching frames — otherwise the
// overlay could come from a newer frame than the displayed image, causing the
// face box/landmarks to drift from the visible face.
std::string EnrollmentWizard::GetLatestFrameAndFaces() {
    std::lock_guard<std::mutex> lock(m_frameCacheMutex);
    std::string result = m_latestFrameB64;
    result += "\x1E";  // record separator
    result += m_latestFacesJson;
    return result;
}

// ============================================================================
// JPEG Encoding via WIC
// ============================================================================

static std::string EncodeBase64(const BYTE* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? tbl[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? tbl[n & 63] : '=');
    }
    return out;
}

std::string EnrollmentWizard::EncodeJPEGBase64(const dlib::matrix<dlib::rgb_pixel>& frame) {
    if (!m_wicFactory) return {};

    int srcW = static_cast<int>(frame.nc());
    int srcH = static_cast<int>(frame.nr());

    std::vector<BYTE> bgra(srcW * srcH * 4);
    for (int y = 0; y < srcH; y++) {
        BYTE* row = bgra.data() + y * srcW * 4;
        for (int x = 0; x < srcW; x++) {
            const auto& p = frame(y, x);
            row[x * 4 + 0] = p.blue;
            row[x * 4 + 1] = p.green;
            row[x * 4 + 2] = p.red;
            row[x * 4 + 3] = 255;
        }
    }

    IWICBitmap* pBitmap = nullptr;
    HRESULT hr = m_wicFactory->CreateBitmapFromMemory(
        srcW, srcH, GUID_WICPixelFormat32bppBGR,
        srcW * 4, static_cast<UINT>(bgra.size()), bgra.data(), &pBitmap);
    if (FAILED(hr)) return {};

    IWICBitmapEncoder* pEncoder = nullptr;
    hr = m_wicFactory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &pEncoder);
    if (FAILED(hr)) { pBitmap->Release(); return {}; }

    IStream* pStream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
    hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { pEncoder->Release(); pStream->Release(); pBitmap->Release(); return {}; }

    IWICBitmapFrameEncode* pFrameEncode = nullptr;
    IPropertyBag2* pProps = nullptr;
    hr = pEncoder->CreateNewFrame(&pFrameEncode, &pProps);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.70f;
        pProps->Write(1, &opt, &v);
        VariantClear(&v);
        pFrameEncode->Initialize(pProps);
        pFrameEncode->SetSize(srcW, srcH);
        pFrameEncode->WriteSource(pBitmap, nullptr);
        pFrameEncode->Commit();
        pEncoder->Commit();
    }

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    ULONG jpgSize = static_cast<ULONG>(stat.cbSize.QuadPart);
    std::vector<BYTE> jpgData(jpgSize);
    LARGE_INTEGER li = {};
    pStream->Seek(li, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    pStream->Read(jpgData.data(), jpgSize, &read);

    std::string result = "data:image/jpeg;base64,";
    result += EncodeBase64(jpgData.data(), jpgSize);

    if (pFrameEncode) pFrameEncode->Release();
    if (pProps) pProps->Release();
    pEncoder->Release();
    pStream->Release();
    pBitmap->Release();

    return result;
}

// ============================================================================
// Face Detection -> JSON
// ============================================================================

std::string EnrollmentWizard::FacesToJson(
    const std::vector<facelogin::FaceWithKps>& faces) {
    std::ostringstream js;
    js << "[";
    for (size_t fi = 0; fi < faces.size(); fi++) {
        if (fi > 0) js << ",";
        const auto& f = faces[fi];
        js << "{"
           << "\"x\":" << f.rect.left()
           << ",\"y\":" << f.rect.top()
           << ",\"w\":" << static_cast<int>(f.rect.width())
           << ",\"h\":" << static_cast<int>(f.rect.height())
           << ",\"landmarks\":[";
        for (int i = 0; i < 5; i++) {
            if (i > 0) js << ",";
            js << static_cast<int>(f.kps[i * 2]) << ","
               << static_cast<int>(f.kps[i * 2 + 1]);
        }
        js << "]";
        // Estimated head yaw in degrees (positive = turned toward own left).
        // Exposed for the preview overlay and multi-angle enrollment gating.
        js << ",\"yaw\":" << EstimateYawDeg(f.kps);
        // While capturing, the JS overlay color-codes the yaw readout against
        // this angle's gate.
        if (m_capturing)
            js << ",\"targetYaw\":" << kAngleTargets[m_captureAngle];
        js << "}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Enrollment actions (called from JS)
// ============================================================================

std::string EnrollmentWizard::GetUsername() const {
    // Return UPN for MSA accounts, SAM name for local accounts
    const std::wstring& display = m_upn.empty() ? m_username : m_upn;
    int len = WideCharToMultiByte(CP_UTF8, 0, display.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, ' ');
    WideCharToMultiByte(CP_UTF8, 0, display.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

bool EnrollmentWizard::CaptureFaceSamples(int angleIndex) {
    if (angleIndex < 0 || angleIndex > 2) {
        FACELOGIN_ERROR(L"CaptureFaceSamples: invalid angle %d", angleIndex);
        return false;
    }

    // COM methods can be invoked independently of the intended HTML flow.
    // Enforce the full capture boundary here so a caller cannot start a side
    // capture after StartPreview failed and thereby bypass anti-spoofing.
    if (!m_previewRunning || m_livenessMethod != LivenessMethod::AntiSpoof ||
        !m_antiSpoof || !m_antiSpoof->IsInitialized() ||
        !m_onnxDetector || !m_onnxDetector->IsInitialized() ||
        !m_onnxRecognizer || !m_onnxRecognizer->IsInitialized()) {
        FACELOGIN_ERROR(L"CaptureFaceSamples refused: preview or mandatory models are unavailable");
        return false;
    }

    // Join the previous capture thread if one exists — REQUIRED before
    // reassigning m_captureThread (an un-joined joinable thread terminates
    // the process). If it is still running (m_capturing stuck true, e.g. the
    // user clicked Start again mid-capture or a prior capture was interrupted
    // by a page switch), wait for it instead of silently returning false —
    // the capture loop has its own fail counters, so this join is bounded
    // (~20s worst case).
    if (m_captureThread.joinable()) {
        if (m_capturing)
            FACELOGIN_WARN(L"CaptureFaceSamples: previous capture still running — waiting for it to finish");
        m_captureThread.join();
        m_capturing = false;
    }

    m_capturing = true;
    m_captureAngle = angleIndex;
    if (angleIndex == 0) {
        // New capture round: reset everything. Later angles append to the
        // flat m_embeddings vector (grouped in angle order).
        m_samplesCollected = 0;
        m_embeddings.clear();
        m_angleSampleCounts[0] = m_angleSampleCounts[1] = m_angleSampleCounts[2] = 0;
    } else {
        // Non-front angle: keep the embeddings captured for EARLIER angles
        // (multi-angle continuation) but drop stale samples left in this
        // angle's slot by a previous round. Without this trim, a single-angle
        // re-capture of the same angle after a save/cancel would see the
        // leftover count == target and the loop below would finish instantly
        // with zero new frames, jumping straight to the confirm screen.
        size_t prior = 0;
        for (int a = 0; a < angleIndex; a++)
            prior += static_cast<size_t>(m_angleSampleCounts[a]);
        if (m_embeddings.size() > prior)
            m_embeddings.resize(prior);
        m_angleSampleCounts[angleIndex] = 0;
    }
    m_livenessPassed = false;
    m_livenessChecking = true;

    m_captureThread = std::thread([this, angleIndex]() {
        // Phase 1: every independently callable capture must prove liveness.
        // In particular, side-angle append is exposed through COM and cannot
        // inherit a stale proof from an earlier front capture or save.
        LivenessMethod method = m_livenessMethod;
        bool livenessPassed = false;
        bool livenessInferenceError = false;
        std::uint64_t lastFrameSequence = 0;

        // The preview and capture threads run independently. Consume each
        // cached camera frame at most once so a stalled preview cannot turn one
        // PAD result into a synthetic 5/5 sequence.
        const auto copyFreshFrame = [this, &lastFrameSequence](
            dlib::matrix<dlib::rgb_pixel>& output) -> bool {
            std::lock_guard<std::mutex> lock(m_frameCacheMutex);
            if (m_latestFrame.size() == 0 ||
                m_latestFrameSequence == lastFrameSequence) {
                return false;
            }
            output = m_latestFrame;
            lastFrameSequence = m_latestFrameSequence;
            return true;
        };

        FACELOGIN_INFO(L"Enrollment: starting mandatory anti-spoof check for angle %d",
                       angleIndex);

        if (method == LivenessMethod::AntiSpoof &&
            m_antiSpoof && m_antiSpoof->IsInitialized()) {
            int totalChecks = AntiSpoofCheckCount(m_antiSpoofThreshold);
            int passRequired = AntiSpoofPassRequired(totalChecks);
            FACELOGIN_INFO(L"Enrollment anti-spoof: threshold=%.3f → %d checks, %d required",
                           m_antiSpoofThreshold, totalChecks, passRequired);
            auto asStart = std::chrono::steady_clock::now();
            int passCount = 0, totalChecked = 0;
            while (m_capturing && totalChecked < totalChecks) {
                auto elapsed = std::chrono::steady_clock::now() - asStart;
                if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 8) break;

                dlib::matrix<dlib::rgb_pixel> frame;
                if (!copyFreshFrame(frame)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                    continue;
                }
                // MiniFASNet consumes expanded crops around the SCRFD bbox.
                auto asDet = m_onnxDetector->DetectLargestFace(frame);
                if (!asDet) { std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }

                float score = m_antiSpoof->Predict(frame,
                    dlib::rectangle(static_cast<long>(asDet->x1),
                                    static_cast<long>(asDet->y1),
                                    static_cast<long>(asDet->x2),
                                    static_cast<long>(asDet->y2)));
                if (!std::isfinite(score) || score < 0.0f || score > 1.0f) {
                    FACELOGIN_ERROR(L"Enrollment anti-spoof returned invalid score: %.4f", score);
                    livenessInferenceError = true;
                    break;
                }

                totalChecked++;
                if (score >= m_antiSpoofThreshold) passCount++; // config-driven threshold
                FACELOGIN_INFO(L"Enrollment anti-spoof frame %d: score=%.3f (pass=%d)",
                              totalChecked, score, passCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            livenessPassed = (!livenessInferenceError &&
                              totalChecked == totalChecks &&
                              passCount >= passRequired);
        } else {
            FACELOGIN_ERROR(L"Enrollment refused: anti-spoof is not initialized");
        }

        m_livenessChecking = false;

        if (!livenessPassed) {
            FACELOGIN_WARN(L"Enrollment liveness check failed");
            m_capturing = false;
            return;
        }

        // Phase 2: collect yaw-gated face samples for this angle. Every frame
        // that contributes to the stored embedding is independently checked
        // by PAD on that exact image. This closes the live-then-photo swap
        // window that exists when liveness and enrollment use disjoint frames.
        const int targetYaw = kAngleTargets[angleIndex];
        constexpr float kYawTolerance = 10.0f;
        constexpr float kMinFaceSizePx = 60.0f;
        constexpr float kMinDetScore = 0.5f;

        int failCount = 0;
        while (m_angleSampleCounts[angleIndex] < kAngleTargetFrames && m_capturing) {
            // Read the latest frame from the frame-grab thread (no camera contention)
            dlib::matrix<dlib::rgb_pixel> frame;
            if (!copyFreshFrame(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                continue;
            }

            // Detect with SCRFD — the 5 keypoints drive the alignment inside
            // ComputeEmbedding (no landmark model).
            auto onnxDet = m_onnxDetector->DetectLargestFace(frame);
            if (!onnxDet) {
                if (++failCount > 600) { m_capturing = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // Yaw gate: only frames near the target angle count as samples.
            m_lastYaw = EstimateYawDeg(onnxDet->kps);
            const float faceW = onnxDet->x2 - onnxDet->x1;
            if (std::abs(m_lastYaw - targetYaw) > kYawTolerance ||
                faceW < kMinFaceSizePx || onnxDet->score < kMinDetScore) {
                failCount = 0;   // waiting for the user to turn — not a failure
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            const float samplePadScore = m_antiSpoof->Predict(frame,
                dlib::rectangle(static_cast<long>(onnxDet->x1),
                                static_cast<long>(onnxDet->y1),
                                static_cast<long>(onnxDet->x2),
                                static_cast<long>(onnxDet->y2)));
            if (!std::isfinite(samplePadScore) || samplePadScore < 0.0f ||
                samplePadScore > 1.0f || samplePadScore < m_antiSpoofThreshold) {
                FACELOGIN_WARN(L"Enrollment sample PAD failed closed for angle %d: score=%.4f",
                               angleIndex, samplePadScore);
                livenessPassed = false;
                break;
            }

            // Compute the embedding with InsightFace ONNX (the only recognizer).
            // Store the FULL 512-D embedding (no truncation).
            auto onnxEmb = m_onnxRecognizer->ComputeEmbedding(frame, onnxDet->kps);
            if (onnxEmb.empty()) {
                if (++failCount > 600) { m_capturing = false; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            dlib::matrix<float, 0, 1> emb;
            emb.set_size(static_cast<long>(onnxEmb.size()));
            for (size_t k = 0; k < onnxEmb.size(); k++)
                emb(static_cast<long>(k)) = onnxEmb[k];

            failCount = 0;
            m_embeddings.push_back(std::move(emb));
            m_angleSampleCounts[angleIndex]++;
            m_samplesCollected++;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        const bool complete = livenessPassed &&
            m_angleSampleCounts[angleIndex] == kAngleTargetFrames;
        m_livenessPassed = complete;
        if (!complete) {
            FACELOGIN_WARN(L"Enrollment capture incomplete for angle %d: %d/%d samples",
                           angleIndex, m_angleSampleCounts[angleIndex].load(),
                           kAngleTargetFrames);
        }
        // Release-publishes the completed embedding vector to SaveEnrollment.
        m_capturing.store(false, std::memory_order_release);
    });

    return true;
}

// Helper: convert wstring to UTF-8 string for JS
static std::string WstrToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

// Helper: UTF-8 string with JSON escaping (quotes, backslashes) — safe to
// embed directly inside a JSON string literal for the face list.
static std::string WstrToUtf8Escaped(const std::wstring& ws) {
    std::string out;
    for (wchar_t ch : ws) {
        if (ch == L'"')       out += "\\\"";
        else if (ch == L'\\') out += "\\\\";
        else {
            char mb[4] = {};
            int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
            if (n > 0) out.append(mb, static_cast<size_t>(n));
        }
    }
    return out;
}

// ============================================================================
// Multi-angle capture status (polled by JS during capture)
// ============================================================================

std::string EnrollmentWizard::GetCaptureStatus() {
    static constexpr const wchar_t* kAngleLabels[3] = {L"正面", L"左转", L"右转"};
    std::ostringstream js;
    const int angle = m_captureAngle.load();
    const int counts[3] = {
        m_angleSampleCounts[0].load(),
        m_angleSampleCounts[1].load(),
        m_angleSampleCounts[2].load()
    };
    const int total = counts[0] + counts[1] + counts[2];
    js << "{\"angle\":" << angle
       << ",\"label\":\"" << WstrToUtf8(kAngleLabels[angle]) << "\""
       << ",\"targetYaw\":" << kAngleTargets[angle]
       << ",\"collected\":" << counts[angle]
       << ",\"target\":" << kAngleTargetFrames
       << ",\"yaw\":" << m_lastYaw
       << ",\"total\":" << total
       << ",\"livenessChecking\":" << (m_livenessChecking ? "true" : "false")
       << ",\"livenessPassed\":" << (m_livenessPassed ? "true" : "false")
       << ",\"done\":" << (m_capturing ? "false" : "true") << "}";
    return js.str();
}

// Notify the FaceLogin service to reload the user database after a write.
static void NotifyServiceReload() {
    HANDLE hPipe = CreateFileW(ipc::PIPE_NAME, GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        std::wstring msg(ipc::MSG_RELOAD_DB);
        msg.push_back(L'\0');
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.size() * sizeof(wchar_t)),
                  &written, nullptr);
        CloseHandle(hPipe);
    }
}

std::string EnrollmentWizard::GetUserSid() const {
    return WstrToUtf8(m_sid);
}

bool EnrollmentWizard::ValidatePassword(const std::wstring& password) {
    // The MSA/local decision MUST come from the live session identity
    // (ResolveSessionUpn), never from the stored record: a registry fallback
    // used to pollute the stored UPN with another account's email
    // (docs/todo.md bug1), which routed local users into the MSA path below
    // and made the account refresh loop impossible to complete.
    std::wstring sessionUpn = ResolveSessionUpn();
    bool isMsa = IsMsaUpn(sessionUpn);

    if (isMsa) {
        // MSA: domain=NULL routes through CloudAP for an online validation of
        // the CURRENT password. (LogonUserW with domain="." would validate
        // against only the local account database — i.e. the STALE cached MSA
        // credential, not the user's current Microsoft password.) INTERACTIVE
        // also refreshes the cached credential when it succeeds (NETWORK
        // never caches).
        HANDLE hToken = nullptr;
        BOOL ok = LogonUserW(sessionUpn.c_str(), nullptr, password.c_str(),
                             LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &hToken);
        if (ok && hToken) {
            CloseHandle(hToken);
            FACELOGIN_INFO(L"ValidatePassword: MSA online validation OK (%s)", sessionUpn.c_str());
            return true;
        }
        DWORD err = GetLastError();
        FACELOGIN_WARN(L"ValidatePassword: MSA interactive logon failed for %s (err=%lu)",
                       sessionUpn.c_str(), err);
        return false;
    }

    // Local account: validate against the local SAM database (reliable offline).
    HANDLE hToken = nullptr;
    BOOL ok = LogonUserW(m_username.c_str(), L".", password.c_str(),
                         LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
    if (ok && hToken) {
        CloseHandle(hToken);
        return true;
    }
    FACELOGIN_WARN(L"ValidatePassword: local logon failed for %s (err=%lu)",
                   m_username.c_str(), GetLastError());
    return false;
}

bool EnrollmentWizard::SaveEnrollment(const std::wstring& password,
                                      const std::wstring& label) {
    return SaveEnrollmentImpl(password, /*passwordless=*/false, label);
}

bool EnrollmentWizard::SaveEnrollmentNoPassword(const std::wstring& label) {
    // Re-verify the current session identity before allowing a passwordless
    // save — the user must be the logged-on owner of this account.
    std::wstring tokenSid = GetCurrentProcessUserSid();
    if (tokenSid.empty() || tokenSid != m_sid) {
        FACELOGIN_ERROR(L"Passwordless enrollment refused: token SID %s != enrolled SID %s",
                        tokenSid.c_str(), m_sid.c_str());
        return false;
    }
    FACELOGIN_INFO(L"Passwordless enrollment confirmed for %s (session identity match)",
                   m_username.c_str());
    return SaveEnrollmentImpl(L"", /*passwordless=*/true, label);
}

// Returns the SID of the currently logged-on session identity (the process
// token's user), used as the "self" proof for passwordless enrollment.
std::wstring EnrollmentWizard::GetCurrentProcessUserSid() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    std::wstring result;
    DWORD sz = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &sz);
    if (sz > 0) {
        std::vector<BYTE> buf(sz);
        if (GetTokenInformation(hToken, TokenUser, buf.data(), sz, &sz)) {
            auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
            LPWSTR sidStr = nullptr;
            if (ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
                result = sidStr;
                LocalFree(sidStr);
            }
        }
    }
    CloseHandle(hToken);
    return result;
}

// Detect whether the enrolled account is passwordless (no password — PIN/Hello
// only). Layered, conservative:
//   1. The current session identity must be the enrolled account.
//   2. Empty-password LogonUser succeeds → definitely passwordless.
//   3. NetUserGetInfo(23) shows an empty SAM password → passwordless.
//   4. MSA that we can't auto-confirm → 2 (UI checkbox lets the user confirm).
int EnrollmentWizard::GetPasswordlessState() const {
    // 1) Session identity must be the account being enrolled.
    std::wstring tokenSid = GetCurrentProcessUserSid();
    if (tokenSid.empty() || tokenSid != m_sid) {
        return 0;
    }

    // 2) Empty-password LogonUser probe.
    HANDLE hToken = nullptr;
    BOOL okEmpty = LogonUserW(m_username.c_str(), L".", L"",
                              LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
    if (okEmpty && hToken) { CloseHandle(hToken); return 1; }
    if (IsMsaUpn(m_upn)) {
        okEmpty = LogonUserW(m_upn.c_str(), L".", L"",
                             LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hToken);
        if (okEmpty && hToken) { CloseHandle(hToken); return 1; }
    }

    // 3) NetUserGetInfo(1003): SAM password field empty → passwordless.
    // (USER_INFO_1003 exposes the SAM password; 23 does not include it.)
    USER_INFO_1003* ui1003 = nullptr;
    if (NetUserGetInfo(nullptr, m_username.c_str(), 1003,
                       reinterpret_cast<LPBYTE*>(&ui1003)) == NERR_Success && ui1003) {
        bool noPw = (ui1003->usri1003_password == nullptr ||
                     ui1003->usri1003_password[0] == L'\0');
        NetApiBufferFree(ui1003);
        if (noPw) return 1;
    }
    // Local accounts: SAM password non-empty → has a password.
    if (m_accountType != "msa") return 0;

    // 4) MSA: SAM doesn't reflect the online password; can't auto-confirm.
    return 2;
}

bool EnrollmentWizard::SaveEnrollmentImpl(const std::wstring& password, bool passwordless,
                                          const std::wstring& label) {
    // Saving is a separate COM entry point, so do not trust the UI to call it
    // only after a completed capture.  A fresh, successful PAD proof is
    // consumed by exactly one save.
    if (m_capturing.load(std::memory_order_acquire) ||
        m_livenessChecking.load() || !m_livenessPassed.load()) {
        FACELOGIN_ERROR(L"Enrollment save refused: live capture has not completed successfully");
        return false;
    }
    if (m_embeddings.empty()) { FACELOGIN_ERROR(L"No face samples"); return false; }

    // Group samples by capture angle. Capture is sequential, so the flat
    // m_embeddings vector is already grouped in angle order.
    struct AngleGroup { size_t begin; size_t count; const wchar_t* label; };
    static constexpr const wchar_t* kAngleLabels[3] = {L"正面", L"左转", L"右转"};
    std::vector<AngleGroup> groups;
    size_t begin = 0;
    for (int a = 0; a < 3; a++) {
        const int count = m_angleSampleCounts[a].load();
        if (count != 0 && count != kAngleTargetFrames) {
            FACELOGIN_ERROR(L"Enrollment save refused: angle %d has %d/%d samples",
                            a, count, kAngleTargetFrames);
            return false;
        }
        if (count > 0)
            groups.push_back({begin, static_cast<size_t>(count), kAngleLabels[a]});
        begin += static_cast<size_t>(count);
    }
    if (groups.empty()) { FACELOGIN_ERROR(L"No face samples"); return false; }
    if (begin != m_embeddings.size()) {
        FACELOGIN_ERROR(L"Enrollment save refused: sample metadata/vector mismatch (%zu != %zu)",
                        begin, m_embeddings.size());
        return false;
    }
    const bool multiAngle = groups.size() > 1;

    // All samples share one dimensionality (enrollment uses one recognizer).
    size_t dim = static_cast<size_t>(m_embeddings[0].size());

    m_store.LoadDatabase();

    // --- Slot pre-check (bug4 fix) ---
    // Two-level cap: kMaxUsers accounts total, kMaxFacesPerUser faces per account.
    // Multi-angle re-enrollment REPLACES old faces (not appends) so it never
    // overflows per-account; single-angle append and new-account creation are
    // checked below.
    size_t existingIdx = m_store.FindUserIndex(m_sid, m_upn, m_username);
    const bool accountExists = existingIdx < m_store.GetUsers().size();
    if (accountExists) {
        if (multiAngle) {
            // Multi-angle re-enrollment: clear old faces, keep account identity.
            if (!m_store.ClearFacesForAccount(m_sid)) {
                FACELOGIN_ERROR(L"Failed to clear existing faces for multi-angle re-enrollment");
                return false;
            }
            // After clearing, the account still exists in m_users (identity +
            // password intact, faces empty). The loop below will find it via
            // FindUserIndex → all angles use the append branch, which preserves
            // the stored password. End result: old faces replaced, account
            // identity untouched.
        } else {
            // Single-angle append: check per-account face slots.
            size_t currentFaces = m_store.GetUsers()[existingIdx].faces.size();
            if (currentFaces + groups.size() > facelogin::kMaxFacesPerUser) {
                FACELOGIN_ERROR(L"Not enough face slots: %s has %zu face(s), "
                               L"need %zu more (max %zu per account). "
                               L"Delete unused faces or re-enroll with multi-angle to replace all.",
                               m_username.c_str(), currentFaces, groups.size(),
                               facelogin::kMaxFacesPerUser);
                return false;
            }
        }
    } else {
        // New account: check global user cap.
        if (m_store.GetUsers().size() >= facelogin::kMaxUsers) {
            FACELOGIN_ERROR(L"Cannot create new account: database has %zu users (max %zu). "
                           L"Delete an unused account first.",
                           m_store.GetUsers().size(), facelogin::kMaxUsers);
            return false;
        }
    }

    // Consistency + averaging are done PER ANGLE GROUP — cross-angle samples
    // are NEVER averaged (the embedding space is pose-sensitive; a cross-angle
    // average drifts toward the match boundary, see docs/side-face-plan-v2).
    for (size_t gi = 0; gi < groups.size(); gi++) {
        const AngleGroup& g = groups[gi];

        // Embedding consistency check: verify all samples in this angle group
        // are from the same person. Compute average pairwise distance — if it
        // exceeds the cap, reject.
        // Same-person distances are typically well below 0.80 (the ONNX
        // boundary); different people exceed it.
        //
        // Same-person cap for 512-D ONNX: 0.80 (measured boundary, credential_store.h).
        // EmbeddingThresholdForDim returns 0.80 for ≥256-D regardless of base;
        // passing 0.80 explicitly avoids the misleading dlib-era 0.45 legacy value.
        {
            double totalDist = 0.0;
            int pairs = 0;
            for (size_t i = g.begin; i < g.begin + g.count; i++) {
                for (size_t j = i + 1; j < g.begin + g.count; j++) {
                    double sum = 0.0;
                    for (size_t k = 0; k < dim; k++) {
                        double diff = m_embeddings[i](static_cast<long>(k)) -
                                      m_embeddings[j](static_cast<long>(k));
                        sum += diff * diff;
                    }
                    totalDist += std::sqrt(sum);
                    pairs++;
                }
            }
            double avgPairDist = (pairs > 0) ? totalDist / pairs : 0.0;
            float maxAllowed = EmbeddingThresholdForDim(0.80f, dim);
            FACELOGIN_INFO(L"Enrollment consistency [%ls]: avg pairwise dist=%.4f (max=%.3f, %d pairs, %zu-D)",
                          g.label, avgPairDist, maxAllowed, pairs, dim);
            if (avgPairDist > maxAllowed) {
                FACELOGIN_ERROR(L"Enrollment consistency check failed [%ls]: avg pairwise dist %.4f > %.3f. "
                               L"Samples may be from different faces.", g.label, avgPairDist, maxAllowed);
                return false;
            }
        }

        // Average this group's samples. Initialize to the samples'
        // dimensionality — the matrix adds require matching sizes.
        dlib::matrix<float, 0, 1> avgEmbedding;
        avgEmbedding.set_size(static_cast<long>(dim));
        avgEmbedding = 0;
        for (size_t i = g.begin; i < g.begin + g.count; i++) {
            const auto& emb = m_embeddings[i];
            if (emb.size() != avgEmbedding.size()) continue;  // defensive
            avgEmbedding += emb;
        }
        avgEmbedding /= static_cast<float>(g.count);

        // Copy the average embedding into a plain float vector (full
        // dimensionality — 512-D for ONNX). Never truncate.
        std::vector<float> ef(static_cast<size_t>(avgEmbedding.size()));
        for (long i = 0; i < avgEmbedding.size(); i++)
            ef[static_cast<size_t>(i)] = avgEmbedding(static_cast<long>(i));

        // create-or-append (1.3.0): the same account may hold several faces.
        // First-time enrollment stores the (protected) password and face #1;
        // subsequent enrollments APPEND a face and leave the stored password
        // untouched (the user is the logged-on session owner, already trusted).
        // In multi-angle mode each angle is one AddFace: the create branch
        // runs for angle 0, the append branch for angles 1-2.
        // Label: multi-angle always uses the angle name. Single-angle keeps a
        // user-provided name, falling back to the angle name (正面/左转/右转)
        // so an appended face is not labeled with the generic 脸N.
        std::wstring groupLabel = label;
        if (multiAngle || groupLabel.empty()) groupLabel = g.label;
        size_t idx = m_store.FindUserIndex(m_sid, m_upn, m_username);
        uint32_t newFaceId = 0;
        if (idx >= m_store.GetUsers().size()) {
            // First face for this account — protect the password now.
            std::vector<uint8_t> protectedPassword;
            if (passwordless) {
                protectedPassword = { facelogin::kPasswordlessSentinelByte };
                FACELOGIN_INFO(L"Storing passwordless enrollment (sentinel) for %s",
                               m_username.c_str());
            } else {
                protectedPassword = DpapiUtil::Protect(
                    reinterpret_cast<const uint8_t*>(password.c_str()),
                    static_cast<UINT>(password.size() * sizeof(wchar_t)));
                if (protectedPassword.empty()) { FACELOGIN_ERROR(L"DPAPI encryption failed"); return false; }
            }
            if (!m_store.AddFace(m_username, m_upn, m_sid, protectedPassword, ef, groupLabel, &newFaceId)) {
                FACELOGIN_ERROR(L"Failed to create enrollment for %s", m_username.c_str());
                return false;
            }
        } else {
            // Append a face to an existing account. AddFace ignores the password
            // argument here, so the stored password/sentinel is preserved.
            if (m_store.GetUsers()[idx].faces.size() >= facelogin::kMaxFacesPerUser) {
                FACELOGIN_ERROR(L"Cannot append: %s already has %zu faces (max %zu)",
                                m_username.c_str(), m_store.GetUsers()[idx].faces.size(),
                                facelogin::kMaxFacesPerUser);
                return false;
            }
            if (!m_store.AddFace(m_username, m_upn, m_sid, {}, ef, groupLabel, &newFaceId)) {
                FACELOGIN_ERROR(L"Failed to append face for %s", m_username.c_str());
                return false;
            }
        }
        FACELOGIN_INFO(L"Enrollment saved for: %s (face #%u, emb=%zu-D%s, angle=%ls)",
                       m_username.c_str(), newFaceId, ef.size(),
                       passwordless ? L", passwordless" : L"", g.label);
    }

    if (!m_store.SaveDatabase()) { FACELOGIN_ERROR(L"Failed to save database"); return false; }

    // Notify service to reload database
    HANDLE hPipe = CreateFileW(ipc::PIPE_NAME, GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        std::wstring msg(ipc::MSG_RELOAD_DB);
        msg.push_back(L'\0');
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.size() * sizeof(wchar_t)),
                  &written, nullptr);
        CloseHandle(hPipe);
    }

    // Capture buffer consumed — reset it so the next capture starts clean.
    // Without this, a re-capture of the same angle would see the leftover
    // sample count and complete instantly with zero new frames (bug4 test).
    m_embeddings.clear();
    m_angleSampleCounts[0] = m_angleSampleCounts[1] = m_angleSampleCounts[2] = 0;
    m_livenessPassed = false;
    m_livenessChecking = false;
    return true;
}


// ============================================================================
// Multi-face management (1.3.0)
// ============================================================================

int EnrollmentWizard::GetFaceCount() {
    m_store.LoadDatabase();
    return static_cast<int>(m_store.GetFaceCount(m_sid));
}

std::string EnrollmentWizard::GetFacesJson() {
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(m_sid, m_upn, m_username);
    if (idx >= m_store.GetUsers().size()) return "[]";

    std::ostringstream js;
    js << "[";
    const auto& faces = m_store.GetUsers()[idx].faces;
    for (size_t i = 0; i < faces.size(); i++) {
        if (i > 0) js << ",";
        const auto& f = faces[i];
        js << "{\"id\":" << f.id
           << ",\"label\":\"" << WstrToUtf8Escaped(f.label) << "\"}";
    }
    js << "]";
    return js.str();
}

bool EnrollmentWizard::SaveEnrollmentAppend(const std::wstring& label) {
    // The appended face belongs to the logged-on session owner — the session
    // token SID must match the enrolled account (same self-proof as the
    // passwordless flow). No password is required for an append.
    std::wstring tokenSid = GetCurrentProcessUserSid();
    if (tokenSid.empty() || tokenSid != m_sid) {
        FACELOGIN_ERROR(L"Face append refused: token SID %s != enrolled SID %s",
                        tokenSid.c_str(), m_sid.c_str());
        return false;
    }
    return SaveEnrollmentImpl(L"", /*passwordless=*/false, label);
}

bool EnrollmentWizard::DeleteFace(int faceId) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    if (!m_store.DeleteFace(m_sid, static_cast<uint32_t>(faceId))) return false;
    if (!m_store.SaveDatabase()) return false;
    NotifyServiceReload();
    return true;
}

bool EnrollmentWizard::ClearAllFaces() {
    m_store.LoadDatabase();
    if (!m_store.ClearAllFaces(m_sid)) return false;
    if (!m_store.SaveDatabase()) return false;
    NotifyServiceReload();
    return true;
}

bool EnrollmentWizard::RenameFace(int faceId, const std::wstring& label) {
    if (faceId <= 0) return false;
    m_store.LoadDatabase();
    if (!m_store.RenameFace(m_sid, static_cast<uint32_t>(faceId), label)) return false;
    if (!m_store.SaveDatabase()) return false;
    NotifyServiceReload();
    return true;
}

// Detect a stale account-type record (symmetric MSA ↔ local). We never trust
// m_upn/m_accountType here — they are derived from the session at construction
// and could be empty for local accounts; instead we re-query the CURRENT
// session identity with ResolveSessionUpn() (docs/todo.md bug1):
//   UPN contains '@'  → current account is MSA
//   empty (after fallback)  → current account is local
// Then compare against the stored record (matched by SID, which Windows keeps
// across MSA↔local conversions):
//   local + record UPN is an MSA email  → state 1 (MSA→local, clear UPN)
//   MSA   + record UPN empty/different  → state 2 (local→MSA, write current UPN)
//   everything else                     → state 0 (no refresh needed)
int EnrollmentWizard::GetAccountTypeChanged() {
    // Determine the CURRENT session's account type via the authoritative
    // query used everywhere else (docs/todo.md bug1). Empty (after fallback)
    // means no MSA identity → local account.
    std::wstring curUpn = ResolveSessionUpn();
    bool sessionIsMsa = IsMsaUpn(curUpn);

    // Match the current identity against stored records (same priority as
    // FindUserIndex: SID > UPN > username).
    m_store.LoadDatabase();
    std::wstring tokenSid = GetCurrentProcessUserSid();
    size_t idx = m_store.FindUserIndex(tokenSid, m_upn, m_username);
    if (idx >= m_store.GetUsers().size()) return 0;  // not enrolled → normal first-time flow

    const auto& rec = m_store.GetUsers()[idx];
    if (!sessionIsMsa) {
        // Current account is local. Flag if the record still carries an MSA email.
        if (rec.upn.find(L'@') != std::wstring::npos) {
            FACELOGIN_INFO(L"GetAccountTypeChanged: stale MSA→local record for %s (UPN=%s, faces=%zu)",
                           rec.username.c_str(), rec.upn.c_str(), rec.faces.size());
            return 1;
        }
        return 0;
    }

    // Current account is MSA. Flag if the record UPN is empty (local-era) or a
    // different email than the current session's MSA identity.
    if (rec.upn.empty() || rec.upn != curUpn) {
        FACELOGIN_INFO(L"GetAccountTypeChanged: stale local→MSA record for %s (stored UPN=%s, current=%s, faces=%zu)",
                       rec.username.c_str(), rec.upn.empty() ? L"<empty>" : rec.upn.c_str(),
                       curUpn.c_str(), rec.faces.size());
        return 2;
    }
    return 0;
}

std::string EnrollmentWizard::CheckAccountTypeChanged() {
    int state = GetAccountTypeChanged();
    if (state == 0) return "{\"state\":0}";

    // Attach the face count so the prompt can say "你已录入 N 张人脸", and for
    // state 2 the current MSA email so the prompt can show what will be written.
    m_store.LoadDatabase();
    std::wstring tokenSid = GetCurrentProcessUserSid();
    size_t idx = m_store.FindUserIndex(tokenSid, m_upn, m_username);
    size_t faces = (idx < m_store.GetUsers().size()) ? m_store.GetUsers()[idx].faces.size() : 0;

    if (state == 2) {
        // Re-derive the current MSA email (same authoritative query).
        std::wstring curUpn = ResolveSessionUpn();
        return "{\"state\":2,\"faces\":" + std::to_string(faces) +
               ",\"upn\":\"" + WstrToUtf8Escaped(curUpn) + "\"}";
    }
    return "{\"state\":1,\"faces\":" + std::to_string(faces) + "}";
}

bool EnrollmentWizard::RefreshAccountIdentity(const std::wstring& password) {
    int state = GetAccountTypeChanged();
    if (state == 0) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: no stale record to refresh");
        return false;
    }
    if (password.empty()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: empty password");
        return false;
    }

    // Validate the CURRENT password before touching the database.
    // ValidatePassword routes by account type: MSA (UPN contains '@') goes
    // through CloudAP online/cached validation, local uses LogonUserW(".").
    if (!ValidatePassword(password)) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: password validation failed");
        return false;
    }

    // Re-encrypt with DPAPI (machine scope — matches first enrollment).
    std::vector<uint8_t> protectedPassword = DpapiUtil::Protect(password);
    if (protectedPassword.empty()) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: DPAPI encryption failed");
        return false;
    }

    std::wstring tokenSid = GetCurrentProcessUserSid();
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(tokenSid, m_upn, m_username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"RefreshAccountIdentity: record vanished before update");
        return false;
    }

    // Write the identity matching the CURRENT account type:
    //   state 1 (MSA→local): clear the old MSA UPN.
    //   state 2 (local→MSA): write the current MSA email (session UPN).
    std::wstring newUpn;
    if (state == 2) {
        newUpn = ResolveSessionUpn();
    }
    // state 1 → newUpn stays empty (local account).

    if (!m_store.UpdateAccountIdentity(idx, m_username, newUpn, tokenSid, protectedPassword)) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: identity update failed");
        return false;
    }
    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"RefreshAccountIdentity: save failed");
        return false;
    }

    NotifyServiceReload();
    FACELOGIN_INFO(L"RefreshAccountIdentity: refreshed identity of %s (UPN=%s%s, faces preserved)",
                   m_username.c_str(),
                   newUpn.empty() ? L"<cleared>" : newUpn.c_str(),
                   state == 2 ? L", MSA" : L", local");
    return true;
}

// Dismiss path for the stale-account prompt. Unlike RefreshAccountIdentity
// this needs NO user input: it only drops the '@'-carrying UPN that a buggy
// build wrote into a LOCAL account's record (docs/todo.md bug1), keeping
// username, SID, faces and the DPAPI-encrypted password byte-for-byte intact.
//   - state 1 (current session local + record carries an MSA email): the
//     email is either a misattribution (another account / converted MSA) —
//     clearing it makes the lock-screen pack domain\username, fixing the
//     silent login failure — or a genuine MSA→local conversion where the user
//     simply doesn't want to re-enter the password; identity is corrected and
//     if the password changed they should still run the full refresh.
//   - state 2 (local→MSA) is deliberately NOT touched: the record needs the
//     CURRENT email + re-encrypted password, which only the refresh provides.
bool EnrollmentWizard::ClearStaleAccountUpn() {
    int state = GetAccountTypeChanged();
    if (state != 1) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: not a stale MSA→local record (state=%d), no-op", state);
        return false;
    }

    std::wstring tokenSid = GetCurrentProcessUserSid();
    m_store.LoadDatabase();
    size_t idx = m_store.FindUserIndex(tokenSid, m_upn, m_username);
    if (idx >= m_store.GetUsers().size()) {
        FACELOGIN_WARN(L"ClearStaleAccountUpn: record vanished before update");
        return false;
    }
    const auto& rec = m_store.GetUsers()[idx];
    if (rec.upn.find(L'@') == std::wstring::npos) {
        FACELOGIN_INFO(L"ClearStaleAccountUpn: record already has no MSA email, no-op");
        return false;
    }

    // Pass the record's own encryptedPassword back unchanged so the stored
    // credential is not re-encrypted (only the identity email is dropped).
    if (!m_store.UpdateAccountIdentity(idx, rec.username, L"", tokenSid, rec.encryptedPassword)) {
        FACELOGIN_ERROR(L"ClearStaleAccountUpn: identity update failed");
        return false;
    }
    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"ClearStaleAccountUpn: save failed");
        return false;
    }

    NotifyServiceReload();
    FACELOGIN_INFO(L"ClearStaleAccountUpn: cleared stale MSA UPN for %s (faces=%zu, password untouched)",
                   rec.username.c_str(), rec.faces.size());
    return true;
}

bool EnrollmentWizard::AutoRepairEmptyUpnOnStartup() {
    // Re-derive the current session's MSA identity (GetUserNameExW + token
    // fallback). Only proceed when we actually have an MSA email to write.
    std::wstring curUpn = ResolveSessionUpn();
    if (!IsMsaUpn(curUpn)) {
        // Local session (or detection inconclusive) — nothing to repair.
        return false;
    }

    std::wstring tokenSid = GetCurrentProcessUserSid();
    if (tokenSid.empty()) {
        FACELOGIN_WARN(L"AutoRepairEmptyUpnOnStartup: could not resolve current session SID, skipping");
        return false;
    }

    m_store.LoadDatabase();
    bool modified = false;
    for (size_t idx = 0; idx < m_store.GetUsers().size(); ++idx) {
        const auto& rec = m_store.GetUsers()[idx];
        // Only THIS session user's own record, and only when its UPN is empty.
        // A non-empty but different UPN may be a genuine re-binding and is left
        // to RefreshAccountIdentity (which requires a password check).
        if (rec.sid != tokenSid) continue;
        if (!rec.upn.empty()) continue;

        // Pass rec.sid (not tokenSid) and the existing encryptedPassword back
        // unchanged so the record stays self-consistent and the stored
        // credential is not re-encrypted; faces are preserved by UpdateAccountIdentity.
        if (!m_store.UpdateAccountIdentity(idx, rec.username, curUpn, rec.sid, rec.encryptedPassword)) {
            FACELOGIN_ERROR(L"AutoRepairEmptyUpnOnStartup: identity update failed for %s", rec.username.c_str());
            continue;
        }
        FACELOGIN_INFO(L"AutoRepairEmptyUpnOnStartup: repaired empty UPN for %s (sid=%s, new upn=%s, faces=%zu, password untouched)",
                       rec.username.c_str(), rec.sid.c_str(), curUpn.c_str(), rec.faces.size());
        modified = true;
    }

    if (!modified) return false;

    if (!m_store.SaveDatabase()) {
        FACELOGIN_ERROR(L"AutoRepairEmptyUpnOnStartup: save failed");
        return false;
    }
    NotifyServiceReload();
    return true;
}

// ============================================================================
// Camera device enumeration
// ============================================================================

std::string EnrollmentWizard::GetCameraList() {
    auto devices = WebcamCapture::ListCameras();
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < devices.size(); i++) {
        if (i > 0) js << ",";
        // JSON-escape name/path (device paths contain backslashes).
        auto esc = [](const std::wstring& ws) -> std::string {
            std::string out;
            for (wchar_t ch : ws) {
                if (ch == L'"' || ch == L'\\') out.push_back('\\');
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) out.append(mb, static_cast<size_t>(n));
            }
            return out;
        };
        js << "{\"path\":\"" << esc(devices[i].devicePath)
           << "\",\"name\":\"" << esc(devices[i].friendlyName) << "\"}";
    }
    js << "]";
    return js.str();
}

// ============================================================================
// Configuration
// ============================================================================

std::string EnrollmentWizard::GetConfig() const {
    return ConfigToJson(m_config);
}

bool EnrollmentWizard::SetConfig(const std::string& json) {
    AppConfig newConfig = ConfigFromJson(json);
    if (newConfig.liveness_method == LivenessMethod::Blink) {
        newConfig.liveness_method = LivenessMethod::AntiSpoof;
    }
    if (newConfig.liveness_method != LivenessMethod::AntiSpoof) {
        FACELOGIN_ERROR(L"SetConfig: unsupported liveness method rejected");
        return false;
    }
    // A running preview must already have the mandatory model.  Refuse to
    // mutate live settings if that invariant has somehow been broken.
    if (m_previewRunning && (!m_antiSpoof || !m_antiSpoof->IsInitialized())) {
        FACELOGIN_ERROR(L"SetConfig: anti-spoof unavailable while preview is running");
        return false;
    }

    bool cameraChanged = (newConfig.camera_device != m_config.camera_device);

    if (!SaveConfig(m_dataDir, newConfig)) {
        FACELOGIN_ERROR(L"Failed to save config.json");
        return false;
    }
    m_config = newConfig;
    m_livenessMethod = newConfig.liveness_method;
    m_antiSpoofThreshold = newConfig.anti_spoof_threshold;

    // Low-light enhancement is recognition-only. PAD stays on its calibrated
    // raw-camera preprocessing path.
    if (m_onnxRecognizer) m_onnxRecognizer->SetLowLightEnhance(newConfig.low_light_enhance);

    // Notify the service and require an explicit acknowledgement. Saving a
    // file is not enough: if the service cannot load the mandatory PAD model,
    // the UI must not claim that authentication settings are active.
    bool reloadConfirmed = false;
    HANDLE hPipe = CreateFileW(ipc::PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        std::wstring msg(ipc::MSG_CONFIG_RELOAD);
        msg.push_back(L'\0');
        const DWORD expectedBytes = static_cast<DWORD>(msg.size() * sizeof(wchar_t));
        BOOL writeOk = WriteFile(hPipe, msg.c_str(), expectedBytes, &written, nullptr);

        if (writeOk && written == expectedBytes) {
            DWORD bytesAvailable = 0;
            for (DWORD waited = 0; waited < 10000; waited += 50) {
                if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
                    break;
                }
                if (bytesAvailable > 0) break;
                Sleep(50);
            }

            if (bytesAvailable > 0) {
                wchar_t responseBuffer[64] = {};
                DWORD bytesRead = 0;
                if (ReadFile(hPipe, responseBuffer,
                             static_cast<DWORD>(sizeof(responseBuffer) - sizeof(wchar_t)),
                             &bytesRead, nullptr) && bytesRead > 0) {
                    size_t responseLength = bytesRead / sizeof(wchar_t);
                    while (responseLength > 0 && responseBuffer[responseLength - 1] == L'\0') {
                        responseLength--;
                    }
                    std::wstring response(responseBuffer, responseLength);
                    reloadConfirmed = (response == ipc::MSG_CONFIG_RELOAD_OK);
                    if (!reloadConfirmed) {
                        FACELOGIN_ERROR(L"Service rejected configuration reload: %s",
                                        response.c_str());
                    }
                }
            }
        }
        CloseHandle(hPipe);
    } else {
        FACELOGIN_ERROR(L"Configuration saved but service is unavailable for reload: %lu",
                        GetLastError());
    }

    FACELOGIN_INFO(L"Configuration updated: rec=%hs det=%hs live=%hs thr=%.2f rotation=%d",
                  m_config.recognition_model.c_str(), m_config.detector.c_str(),
                  LivenessMethodToString(m_config.liveness_method).c_str(),
                  m_config.match_threshold, m_config.camera_rotation);

    // If the camera selection changed and the preview is running, restart the
    // preview so the new camera takes effect immediately.
    if (cameraChanged && m_previewRunning) {
        FACELOGIN_INFO(L"Camera selection changed — restarting preview");
        RestartPreview();
    }

    return reloadConfirmed;
}

bool EnrollmentWizard::RestartPreview() {
    StopPreview();
    return StartPreview();
}

// ============================================================================
// Log Viewer
// ============================================================================

std::string EnrollmentWizard::GetLogLines() {
    auto lines = Logger::Instance().GetRecentLogs(500);
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) ss << ",";
        ss << "\"";
        for (wchar_t ch : lines[i]) {
            if (ch == L'\\') ss << "\\\\";
            else if (ch == L'"') ss << "\\\"";
            else if (ch == L'\r' || ch == L'\n') {} // strip newlines — JS renders as <div>
            else {
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) ss.write(mb, n);
            }
        }
        ss << "\"";
    }
    ss << "]";
    return ss.str();
}

std::string EnrollmentWizard::GetServiceLogLines() {
    // Read the service log file directly — avoids pipe message size limits.
    // The log file is written in UTF-16LE (wchar_t on Windows).
    std::wstring logPath = m_dataDir + L"\\log\\service.log";
    HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return "[\"Service log file not available\"]";
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize < 2) {
        CloseHandle(hFile);
        return "[\"Service log is empty\"]";
    }

    // Cap at ~256KB of raw bytes
    DWORD capSize = fileSize;
    if (capSize > 256 * 1024) capSize = 256 * 1024;

    std::vector<wchar_t> wbuf(capSize / sizeof(wchar_t) + 1);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, wbuf.data(), capSize, &bytesRead, nullptr) || bytesRead < 2) {
        CloseHandle(hFile);
        return "[\"Failed to read service log\"]";
    }
    CloseHandle(hFile);

    size_t wlen = bytesRead / sizeof(wchar_t);

    // Parse lines: each log line ends with \r\n (wchar_t)
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    size_t pos = 0;
    while (pos < wlen) {
        // Find end of line
        size_t lineStart = pos;
        while (pos < wlen && wbuf[pos] != L'\r' && wbuf[pos] != L'\n') pos++;
        size_t lineLen = pos - lineStart;
        // Skip \r\n
        while (pos < wlen && (wbuf[pos] == L'\r' || wbuf[pos] == L'\n')) pos++;
        if (lineLen == 0) continue;

        if (!first) ss << ",";
        first = false;
        ss << "\"";
        for (size_t i = 0; i < lineLen; i++) {
            wchar_t ch = wbuf[lineStart + i];
            if (ch == L'\\') ss << "\\\\";
            else if (ch == L'"') ss << "\\\"";
            else if (ch >= 0x20 && ch < 0x7F) ss << static_cast<char>(ch);
            else {
                // Non-ASCII character — convert via UTF-8
                char mb[4] = {};
                int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 4, nullptr, nullptr);
                if (n > 0) ss.write(mb, n);
            }
        }
        ss << "\"";
    }
    ss << "]";
    return ss.str();
}

void EnrollmentWizard::ClearLog() {
    Logger::Instance().ClearLogs();
}

} // namespace facelogin
