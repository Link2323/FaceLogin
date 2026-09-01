#include "webcam_capture_dshow.h"
#include "../common/logger.h"
#include <oaidl.h>
#include <oleauto.h>
#include <new>
#include <utility>

#pragma comment(lib, "strmiids.lib")

namespace facelogin {

// ============================================================================
// Static COM helpers
// ============================================================================

thread_local bool WebcamCaptureDS::s_comOwned = false;
thread_local int  WebcamCaptureDS::s_comRefCount = 0;

bool WebcamCaptureDS::InitializeCOM() {
    if (s_comRefCount == 0) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // WebView2 initializes FaceLoginConsole's UI thread as STA. Do
            // not try to change it to MTA: DirectShow operates correctly in
            // that existing apartment, which remains owned by the UI host.
            FACELOGIN_INFO(L"DS using existing COM apartment");
        } else if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS CoInitializeEx(COINIT_MULTITHREADED) failed: 0x%08X", hr);
            return false;
        } else {
            s_comOwned = true;
        }
    }
    s_comRefCount++;
    return true;
}

void WebcamCaptureDS::ShutdownCOM() {
    if (s_comRefCount > 0) {
        s_comRefCount--;
        if (s_comRefCount == 0 && s_comOwned) {
            CoUninitialize();
            s_comOwned = false;
        }
    }
}

// ============================================================================
// Construction / destruction
// ============================================================================

WebcamCaptureDS::WebcamCaptureDS()
    : m_callback(this)
{
    InitializeCriticalSection(&m_frameCs);
}

WebcamCaptureDS::~WebcamCaptureDS() {
    Shutdown();
    DeleteCriticalSection(&m_frameCs);
}

// ============================================================================
// SampleCB — ISampleGrabberCB
// ============================================================================

STDMETHODIMP WebcamCaptureDS::GrabberCB::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
        *ppv = static_cast<ISampleGrabberCB*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) WebcamCaptureDS::GrabberCB::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) WebcamCaptureDS::GrabberCB::Release() {
    return InterlockedDecrement(&m_refCount);
}

STDMETHODIMP WebcamCaptureDS::GrabberCB::SampleCB(double, IMediaSample*) {
    return S_OK;
}

STDMETHODIMP WebcamCaptureDS::GrabberCB::BufferCB(double, BYTE* buffer, long len) {
    if (!buffer || len <= 0)
        return S_OK;

    EnterCriticalSection(&m_parent->m_frameCs);

    if (m_parent->m_frameSize < len) {
        delete[] m_parent->m_frameBuffer;
        m_parent->m_frameBuffer = new (std::nothrow) BYTE[len];
        if (!m_parent->m_frameBuffer) {
            LeaveCriticalSection(&m_parent->m_frameCs);
            return E_OUTOFMEMORY;
        }
        m_parent->m_frameSize = len;
    }

    memcpy(m_parent->m_frameBuffer, buffer, len);
    m_parent->m_frameReady = true;
    ++m_parent->m_frameSeq;

    LeaveCriticalSection(&m_parent->m_frameCs);
    return S_OK;
}

// ============================================================================
// Camera enumeration
// ============================================================================

static bool GetPin(IBaseFilter* pFilter, PIN_DIRECTION dir, IPin** ppPin) {
    *ppPin = nullptr;
    IEnumPins* pEnum = nullptr;
    if (FAILED(pFilter->EnumPins(&pEnum))) return false;

    IPin* pPin = nullptr;
    while (pEnum->Next(1, &pPin, nullptr) == S_OK) {
        PIN_DIRECTION pinDir;
        if (SUCCEEDED(pPin->QueryDirection(&pinDir)) && pinDir == dir) {
            *ppPin = pPin;
            pEnum->Release();
            return true;
        }
        pPin->Release();
    }
    pEnum->Release();
    return false;
}

bool WebcamCaptureDS::FindMoniker(const std::wstring& devicePath,
                                  IMoniker** ppMoniker) {
    *ppMoniker = nullptr;

    ICreateDevEnum* pDevEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pDevEnum));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(SystemDeviceEnum) failed: 0x%08X", hr);
        return false;
    }

    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    pDevEnum->Release();

    if (FAILED(hr) || pEnum == nullptr) {
        FACELOGIN_WARN(L"DS: no video capture devices found (pEnum=%p, hr=0x%08X)",
                       (void*)pEnum, hr);
        return false;
    }

    // Match the configured DirectShow DevicePath; otherwise keep the first
    // moniker as the fallback choice.
    IMoniker* pMatch = nullptr;
    IMoniker* pFirst = nullptr;
    IMoniker* pMoniker = nullptr;
    ULONG fetched = 0;
    while (pEnum->Next(1, &pMoniker, &fetched) == S_OK && fetched == 1) {
        if (!pFirst) {
            pFirst = pMoniker;
            pFirst->AddRef();
        }
        if (!devicePath.empty() && !pMatch) {
            IPropertyBag* pBag = nullptr;
            if (SUCCEEDED(pMoniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&pBag)))) {
                VARIANT var; VariantInit(&var);
                if (SUCCEEDED(pBag->Read(L"DevicePath", &var, nullptr)) && var.vt == VT_BSTR) {
                    std::wstring dsPath = var.bstrVal;
                    if (devicePath == dsPath) {
                        pMatch = pMoniker;
                        pMatch->AddRef();
                    }
                }
                VariantClear(&var);
                pBag->Release();
            }
        }
        pMoniker->Release();
        fetched = 0;
    }
    pEnum->Release();

    IMoniker* chosen = nullptr;
    if (pMatch) {
        chosen = pMatch;
        if (pFirst) pFirst->Release();
        FACELOGIN_INFO(L"DS: using configured camera (matched DevicePath)");
    } else if (pFirst) {
        chosen = pFirst;
        if (!devicePath.empty()) {
            FACELOGIN_WARN(L"DS: configured camera not found — falling back to first device");
        }
    } else {
        FACELOGIN_WARN(L"DS: no camera monikers available");
        return false;
    }
    *ppMoniker = chosen;
    return true;
}

bool WebcamCaptureDS::BindMoniker(IMoniker* pMoniker, IBaseFilter** ppFilter) {
    *ppFilter = nullptr;
    const HRESULT hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter,
                                               (void**)ppFilter);
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS BindToObject (camera) failed: 0x%08X", hr);
        return false;
    }
    // Graph-build step detail — DEBUG; the final "DirectShow webcam
    // initialized" line at the end of Initialize() is the INFO-level summary.
    FACELOGIN_DEBUG(L"DS: bound video capture device");
    return true;
}

bool WebcamCaptureDS::FindCamera(const std::wstring& devicePath,
                                 IBaseFilter** ppFilter) {
    IMoniker* pMoniker = nullptr;
    if (!FindMoniker(devicePath, &pMoniker)) {
        return false;
    }
    const bool bound = BindMoniker(pMoniker, ppFilter);
    pMoniker->Release();
    return bound;
}

std::vector<CameraDeviceInfo> WebcamCaptureDS::ListCameras() {
    std::vector<CameraDeviceInfo> devices;
    if (!InitializeCOM()) return devices;

    ICreateDevEnum* devEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) {
        ShutdownCOM();
        return devices;
    }

    IEnumMoniker* enumerator = nullptr;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                        &enumerator, 0);
    devEnum->Release();
    if (FAILED(hr) || !enumerator) {
        ShutdownCOM();
        return devices;
    }

    IMoniker* moniker = nullptr;
    ULONG fetched = 0;
    while (enumerator->Next(1, &moniker, &fetched) == S_OK && fetched == 1) {
        IPropertyBag* bag = nullptr;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr,
                                             IID_PPV_ARGS(&bag)))) {
            CameraDeviceInfo info;
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(bag->Read(L"DevicePath", &value, nullptr)) &&
                value.vt == VT_BSTR && value.bstrVal) {
                info.devicePath = value.bstrVal;
            }
            VariantClear(&value);
            VariantInit(&value);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) &&
                value.vt == VT_BSTR && value.bstrVal) {
                info.friendlyName = value.bstrVal;
            }
            VariantClear(&value);
            bag->Release();
            if (!info.devicePath.empty() || !info.friendlyName.empty()) {
                devices.push_back(std::move(info));
            }
        }
        moniker->Release();
        moniker = nullptr;
        fetched = 0;
    }
    enumerator->Release();
    ShutdownCOM();
    return devices;
}

// ============================================================================
// Filter graph construction
// ============================================================================

// Device-independent half of graph construction: FilterGraph, the RGB24
// SampleGrabber and the Null Renderer. Touches no capture device, so it is
// safe to run during lock-screen preload.
bool WebcamCaptureDS::BuildSkeleton() {
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&m_pGraph));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(FilterGraph) failed: 0x%08X", hr);
        return false;
    }

    IBaseFilter* pGrabberFilter = nullptr;
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&pGrabberFilter));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(SampleGrabber) failed: 0x%08X", hr);
        return false;
    }

    hr = m_pGraph->AddFilter(pGrabberFilter, L"Sample Grabber");
    if (FAILED(hr)) {
        pGrabberFilter->Release();
        return false;
    }

    hr = pGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&m_pGrabber);
    pGrabberFilter->Release();  // graph owns the ref now
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS QueryInterface(ISampleGrabber) failed: 0x%08X", hr);
        return false;
    }

    // Configure Sample Grabber: RGB24, BufferCB (not buffered)
    {
        AM_MEDIA_TYPE mt = {};
        mt.majortype  = MEDIATYPE_Video;
        mt.subtype    = MEDIASUBTYPE_RGB24;
        mt.formattype = FORMAT_VideoInfo;
        hr = m_pGrabber->SetMediaType(&mt);
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS SetMediaType(RGB24) failed: 0x%08X", hr);
            return false;
        }

        hr = m_pGrabber->SetBufferSamples(FALSE);
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS SetBufferSamples(FALSE) failed: 0x%08X", hr);
            return false;
        }

        hr = m_pGrabber->SetCallback(&m_callback, 1);  // 1 = BufferCB
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS SetCallback failed: 0x%08X", hr);
            return false;
        }
    }

    hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&m_pNullRenderer));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(NullRenderer) failed: 0x%08X", hr);
        return false;
    }

    hr = m_pGraph->AddFilter(m_pNullRenderer, L"Null Renderer");
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS AddFilter(NullRenderer) failed: 0x%08X", hr);
        return false;
    }

    return true;
}

// Device-dependent half: add the capture filter, negotiate the resolution,
// manually connect  Capture → SampleGrabber → Null Renderer,  and query
// IMediaControl. Data only flows once the caller Run()s the graph.
bool WebcamCaptureDS::AttachCapture(IBaseFilter* pCapture, int width, int height) {
    HRESULT hr = m_pGraph->AddFilter(pCapture, L"Video Capture");
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS AddFilter(capture) failed: 0x%08X", hr);
        return false;
    }

    // Try to set resolution via IAMStreamConfig
    {
        IPin* pCaptureOut = nullptr;
        if (GetPin(pCapture, PINDIR_OUTPUT, &pCaptureOut)) {
            IAMStreamConfig* pConfig = nullptr;
            hr = pCaptureOut->QueryInterface(IID_PPV_ARGS(&pConfig));
            if (SUCCEEDED(hr)) {
                AM_MEDIA_TYPE* pmt = nullptr;
                hr = pConfig->GetFormat(&pmt);
                if (SUCCEEDED(hr) && pmt) {
                    if (pmt->formattype == FORMAT_VideoInfo) {
                        auto* pVih = (VIDEOINFOHEADER*)pmt->pbFormat;
                        pVih->bmiHeader.biWidth  = width;
                        pVih->bmiHeader.biHeight = height;
                        pVih->bmiHeader.biSizeImage = width * height * 3;
                        hr = pConfig->SetFormat(pmt);
                        if (SUCCEEDED(hr))
                            FACELOGIN_DEBUG(L"DS: set capture format %dx%d", width, height);
                        else
                            FACELOGIN_WARN(L"DS SetFormat failed: 0x%08X, using default", hr);
                    }
                    FreeMediaType(*pmt);
                    CoTaskMemFree(pmt);
                }
                pConfig->Release();
            }
            pCaptureOut->Release();
        }
    }

    // Manually connect the graph:
    //    Capture output → SampleGrabber input
    //    SampleGrabber output → Null Renderer input
    // This ensures data flows through the SampleGrabber so BufferCB fires.
    {
        IPin* pGrabberIn  = nullptr;
        IPin* pGrabberOut = nullptr;
        IPin* pNullIn     = nullptr;
        IBaseFilter* pGrabberFilter = nullptr;

        // The skeleton added the grabber under a known name; recover its
        // IBaseFilter from the graph for pin lookup.
        hr = m_pGraph->FindFilterByName(L"Sample Grabber", &pGrabberFilter);
        if (FAILED(hr) || !pGrabberFilter) {
            FACELOGIN_ERROR(L"DS: Sample Grabber filter missing from graph");
            if (pGrabberFilter) pGrabberFilter->Release();
            return false;
        }

        if (!GetPin(pGrabberFilter,  PINDIR_INPUT,  &pGrabberIn)  ||
            !GetPin(pGrabberFilter,  PINDIR_OUTPUT, &pGrabberOut) ||
            !GetPin(m_pNullRenderer, PINDIR_INPUT,  &pNullIn)) {
            FACELOGIN_ERROR(L"DS: failed to get pins for graph connection");
            pGrabberFilter->Release();
            if (pGrabberIn) pGrabberIn->Release();
            if (pGrabberOut) pGrabberOut->Release();
            if (pNullIn) pNullIn->Release();
            return false;
        }
        pGrabberFilter->Release();

        IPin* pCaptureOut = nullptr;
        if (!GetPin(pCapture, PINDIR_OUTPUT, &pCaptureOut)) {
            FACELOGIN_ERROR(L"DS: failed to get pins for graph connection");
            pGrabberIn->Release(); pGrabberOut->Release(); pNullIn->Release();
            return false;
        }

        // Connect capture → grabber
        hr = m_pGraph->Connect(pCaptureOut, pGrabberIn);
        pCaptureOut->Release();
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS Connect(capture→grabber) failed: 0x%08X", hr);
            pGrabberIn->Release();
            pGrabberOut->Release();
            pNullIn->Release();
            return false;
        }
        pGrabberIn->Release();

        // Connect grabber → null renderer
        hr = m_pGraph->Connect(pGrabberOut, pNullIn);
        pGrabberOut->Release();
        pNullIn->Release();
        if (FAILED(hr)) {
            FACELOGIN_ERROR(L"DS Connect(grabber→null) failed: 0x%08X", hr);
            return false;
        }

        FACELOGIN_DEBUG(L"DS: graph connected capture→grabber→null");
    }

    // Query IMediaControl
    hr = m_pGraph->QueryInterface(IID_PPV_ARGS(&m_pControl));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS QueryInterface(IMediaControl) failed: 0x%08X", hr);
        return false;
    }

    return true;
}

// ============================================================================
// Public API
// ============================================================================

bool WebcamCaptureDS::Preload(const std::wstring& devicePath) {
    if (m_initialized || m_preloadValid) return m_preloadValid;

    if (!InitializeCOM()) {
        FACELOGIN_ERROR(L"DS: COM init failed (preload)");
        return false;
    }
    if (!FindMoniker(devicePath, &m_pMoniker)) {
        ShutdownCOM();
        return false;
    }
    if (!BuildSkeleton()) {
        FACELOGIN_WARN(L"DS: graph skeleton build failed — full init at AUTH_START");
        DiscardPreload();
        ShutdownCOM();
        return false;
    }
    m_preloadDevicePath = devicePath;
    m_preloadValid = true;
    FACELOGIN_INFO(L"DS preload: camera moniker resolved + graph skeleton ready "
                   L"(device not activated)");
    return true;
}

void WebcamCaptureDS::DiscardPreload() {
    if (m_pControl) { m_pControl->Release(); m_pControl = nullptr; }
    if (m_pGrabber) { m_pGrabber->Release(); m_pGrabber = nullptr; }
    if (m_pNullRenderer) { m_pNullRenderer->Release(); m_pNullRenderer = nullptr; }
    if (m_pCapture) { m_pCapture->Release(); m_pCapture = nullptr; }
    if (m_pGraph) { m_pGraph->Release(); m_pGraph = nullptr; }
    if (m_pMoniker) { m_pMoniker->Release(); m_pMoniker = nullptr; }
    m_preloadDevicePath.clear();
    m_preloadValid = false;
}

bool WebcamCaptureDS::Initialize(int preferredWidth, int preferredHeight,
                                 const std::wstring& devicePath) {
    if (m_initialized) return true;

    m_width  = preferredWidth;
    m_height = preferredHeight;

    if (!InitializeCOM()) {
        FACELOGIN_ERROR(L"DS: COM init failed");
        return false;
    }

    // Prefer the preload (registry enumeration + graph skeleton) built while
    // the desktop was locked: BindToObject — the device ACTIVATION — runs
    // here for the first time. A stale moniker (device list changed while
    // locked) re-enumerates; a preload mismatch or failure falls through to
    // the full legacy construction.
    if (m_preloadValid && devicePath == m_preloadDevicePath) {
        if (BindMoniker(m_pMoniker, &m_pCapture)) {
            // The activation is done; the moniker (a device-registry pointer)
            // is no longer needed while the graph streams.
            m_pMoniker->Release();
            m_pMoniker = nullptr;
        } else {
            FACELOGIN_WARN(L"DS: preloaded moniker no longer binds — re-enumerating");
            IMoniker* fresh = nullptr;
            const bool rebound = FindMoniker(devicePath, &fresh) &&
                                 BindMoniker(fresh, &m_pCapture);
            if (fresh) fresh->Release();
            if (!rebound) DiscardPreload();
        }
    } else {
        DiscardPreload();
    }

    if (!m_pGraph && !BuildSkeleton()) {
        FACELOGIN_ERROR(L"DS: failed to build capture graph");
        DiscardPreload();
        ShutdownCOM();
        return false;
    }
    if (!m_pCapture && !FindCamera(devicePath, &m_pCapture)) {
        FACELOGIN_ERROR(L"DS: no camera found — check if camera is connected and "
                        L"drivers are installed.");
        DiscardPreload();
        ShutdownCOM();
        return false;
    }
    if (!AttachCapture(m_pCapture, m_width, m_height)) {
        FACELOGIN_ERROR(L"DS: failed to build capture graph");
        DiscardPreload();
        ShutdownCOM();
        return false;
    }

    HRESULT hr = m_pControl->Run();
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS IMediaControl::Run failed: 0x%08X", hr);
        Shutdown();
        return false;
    }

    m_initialized = true;
    FACELOGIN_INFO(L"DirectShow webcam initialized: %dx%d RGB24%s",
                   m_width, m_height,
                   m_preloadValid ? L" (preloaded skeleton)" : L"");
    return true;
}

void WebcamCaptureDS::Pause() {
    if (m_pControl && m_initialized) {
        m_pControl->Stop();
        // Discard any stale frame so the next resume starts fresh
        EnterCriticalSection(&m_frameCs);
        m_frameReady = false;
        LeaveCriticalSection(&m_frameCs);
        FACELOGIN_INFO(L"DS: graph stopped (camera LED off)");
    }
}

bool WebcamCaptureDS::GrabFrame(FrameImage& outFrame, unsigned long long* frameSequence) {
    if (!m_initialized) return false;

    EnterCriticalSection(&m_frameCs);

    if (!m_frameReady || !m_frameBuffer || m_frameSize == 0) {
        LeaveCriticalSection(&m_frameCs);
        return false;
    }

    long stride = m_width * 3;
    outFrame.set_size(m_height, m_width);

    // DirectShow RGB24 is bottom-up (biHeight > 0 in VIDEOINFOHEADER
    // means the first scan line is the bottom of the image).
    // FrameImage uses top-down indexing, so we flip vertically.
    //
    // DShow RGB24 byte order: B, G, R
    // RgbPixel struct order: red, green, blue → mem layout = R, G, B
    // → need to swap R↔B

    const BYTE* src = m_frameBuffer;
    for (int row = 0; row < m_height; row++) {
        int srcRow = m_height - 1 - row;  // flip bottom-up → top-down
        const BYTE* srcRowPtr = src + srcRow * stride;
        for (int col = 0; col < m_width; col++) {
            const BYTE* pixel = srcRowPtr + col * 3;
            RgbPixel& dst = outFrame(row, col);
            dst.red   = pixel[2];   // byte 2 of BGR = R
            dst.green = pixel[1];   // byte 1 of BGR = G
            dst.blue  = pixel[0];   // byte 0 of BGR = B
        }
    }

    if (frameSequence) *frameSequence = m_frameSeq;

    LeaveCriticalSection(&m_frameCs);
    return true;
}

void WebcamCaptureDS::Shutdown() {
    // Stop streaming first so BufferCB stops firing
    if (m_pControl) {
        m_pControl->Stop();
        m_pControl->Release();
        m_pControl = nullptr;
    }

    // Break the callback reference before releasing the graph. Relying on the
    // filter's final destruction to detach it retained callback/filter state on
    // some camera drivers across repeated auth cycles.
    if (m_pGrabber) {
        m_pGrabber->SetCallback(nullptr, 1);
        m_pGrabber->Release();
        m_pGrabber = nullptr;
    }
    if (m_pNullRenderer) {
        m_pNullRenderer->Release();
        m_pNullRenderer = nullptr;
    }
    if (m_pCapture) {
        m_pCapture->Release();
        m_pCapture = nullptr;
    }
    if (m_pGraph) {
        m_pGraph->Release();
        m_pGraph = nullptr;
    }
    if (m_pMoniker) {
        m_pMoniker->Release();
        m_pMoniker = nullptr;
    }
    m_preloadDevicePath.clear();
    m_preloadValid = false;

    ShutdownCOM();

    EnterCriticalSection(&m_frameCs);
    delete[] m_frameBuffer;
    m_frameBuffer = nullptr;
    m_frameSize = 0;
    m_frameReady = false;
    m_initialized = false;
    LeaveCriticalSection(&m_frameCs);
}

} // namespace facelogin
