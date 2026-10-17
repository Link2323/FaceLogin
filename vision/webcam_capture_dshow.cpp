#include "webcam_capture_dshow.h"
#include "../common/logger.h"
#include <oaidl.h>
#include <oleauto.h>
#include <new>
#include <utility>

#pragma comment(lib, "strmiids.lib")

namespace facelogin {

using Microsoft::WRL::ComPtr;

namespace {
LONGLONG CaptureTicks() {
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}

double CaptureMilliseconds(LONGLONG start, LONGLONG end) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return 1000.0 * static_cast<double>(end - start) / frequency.QuadPart;
}
} // namespace

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

    const auto callbackAt = CaptureTicks();
    EnterCriticalSection(&m_parent->m_frameCs);
    const bool firstCallback = m_parent->m_firstCallbackTicks == 0;

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
    if (firstCallback) {
        m_parent->m_firstCallbackTicks = callbackAt;
        m_parent->m_firstBufferReadyTicks = CaptureTicks();
    }

    LeaveCriticalSection(&m_parent->m_frameCs);
    return S_OK;
}

// ============================================================================
// Camera enumeration
// ============================================================================

static bool GetPin(IBaseFilter* pFilter, PIN_DIRECTION dir, ComPtr<IPin>& pin) {
    ComPtr<IEnumPins> enumPins;
    if (FAILED(pFilter->EnumPins(enumPins.GetAddressOf()))) return false;

    ComPtr<IPin> current;
    while (enumPins->Next(1, current.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        PIN_DIRECTION pinDir;
        if (SUCCEEDED(current->QueryDirection(&pinDir)) && pinDir == dir) {
            pin = std::move(current);
            return true;
        }
    }
    return false;
}

bool WebcamCaptureDS::FindMoniker(const std::wstring& devicePath,
                                  ComPtr<IMoniker>& chosen) {
    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(SystemDeviceEnum) failed: 0x%08X", hr);
        return false;
    }

    ComPtr<IEnumMoniker> enumMoniker;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                        enumMoniker.GetAddressOf(), 0);
    if (FAILED(hr) || !enumMoniker) {
        FACELOGIN_WARN(L"DS: no video capture devices found (pEnum=%p, hr=0x%08X)",
                       (void*)enumMoniker.Get(), hr);
        return false;
    }

    // Match the configured DirectShow DevicePath. A configured path is
    // authoritative: right after boot the configured camera can drop off the
    // enumeration for tens of seconds (USB re-enumeration churn, observed
    // 2026-09-13: matched at preload, 0x80070003 on activation ~12 s later,
    // back in enumeration ~10 s after that). Falling back to the first
    // moniker then silently put a DIFFERENT camera on the auth critical path
    // — two guaranteed empty-scene failures per cold boot — so an absent
    // configured camera now fails fast into the existing "摄像头不可用"
    // report; only a host with no camera configured at all may take the
    // first device.
    ComPtr<IMoniker> match;
    std::wstring matchName;
    ComPtr<IMoniker> first;
    std::wstring firstName;
    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    while (enumMoniker->Next(1, moniker.ReleaseAndGetAddressOf(), &fetched) == S_OK &&
           fetched == 1) {
        ComPtr<IPropertyBag> bag;
        std::wstring friendly;
        std::wstring dsPath;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
            VARIANT var; VariantInit(&var);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR) {
                friendly = var.bstrVal;
            }
            VariantClear(&var);
            if (!devicePath.empty()) {
                if (SUCCEEDED(bag->Read(L"DevicePath", &var, nullptr)) && var.vt == VT_BSTR) {
                    dsPath = var.bstrVal;
                }
                VariantClear(&var);
            }
        }
        if (!first) {
            first = moniker;
            firstName = friendly;
        }
        if (!devicePath.empty() && !match && dsPath == devicePath) {
            match = moniker;
            matchName = friendly;
        }
        fetched = 0;
    }

    if (match) {
        chosen = std::move(match);
        FACELOGIN_INFO(L"DS: using configured camera (matched DevicePath, \"%s\")",
                       matchName.c_str());
    } else if (first && devicePath.empty()) {
        chosen = std::move(first);
        FACELOGIN_INFO(L"DS: no camera configured — using first device (\"%s\")",
                       firstName.c_str());
    } else {
        FACELOGIN_WARN(L"DS: configured camera not found in enumeration "
                       L"(device absent or not ready)");
        return false;
    }
    return true;
}

bool WebcamCaptureDS::BindMoniker(IMoniker* pMoniker, ComPtr<IBaseFilter>& filter) {
    IBaseFilter* raw = nullptr;
    const HRESULT hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter,
                                               (void**)&raw);
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS BindToObject (camera) failed: 0x%08X", hr);
        return false;
    }
    filter.Attach(raw);
    // Graph-build step detail — DEBUG; the final "DirectShow webcam
    // initialized" line at the end of Initialize() is the INFO-level summary.
    FACELOGIN_DEBUG(L"DS: bound video capture device");
    return true;
}

bool WebcamCaptureDS::FindCamera(const std::wstring& devicePath,
                                 ComPtr<IBaseFilter>& filter) {
    ComPtr<IMoniker> moniker;
    if (!FindMoniker(devicePath, moniker)) {
        return false;
    }
    return BindMoniker(moniker.Get(), filter);
}

std::vector<CameraDeviceInfo> WebcamCaptureDS::ListCameras() {
    std::vector<CameraDeviceInfo> devices;
    if (!InitializeCOM()) return devices;

    ComPtr<ICreateDevEnum> devEnum;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) {
        ShutdownCOM();
        return devices;
    }

    ComPtr<IEnumMoniker> enumerator;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                        enumerator.GetAddressOf(), 0);
    if (FAILED(hr) || !enumerator) {
        ShutdownCOM();
        return devices;
    }

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    while (enumerator->Next(1, moniker.ReleaseAndGetAddressOf(), &fetched) == S_OK &&
           fetched == 1) {
        ComPtr<IPropertyBag> bag;
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
            if (!info.devicePath.empty() || !info.friendlyName.empty()) {
                devices.push_back(std::move(info));
            }
        }
        fetched = 0;
    }
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

    ComPtr<IBaseFilter> grabberFilter;
    hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&grabberFilter));
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS CoCreateInstance(SampleGrabber) failed: 0x%08X", hr);
        return false;
    }

    hr = m_pGraph->AddFilter(grabberFilter.Get(), L"Sample Grabber");
    if (FAILED(hr)) {
        return false;
    }

    // ISampleGrabber is the locally declared minimal interface (Win10+ SDK
    // dropped it), so QI goes through its explicit IID — no __uuidof.
    hr = grabberFilter->QueryInterface(
        IID_ISampleGrabber, reinterpret_cast<void**>(m_pGrabber.ReleaseAndGetAddressOf()));
    // grabberFilter releases on scope exit — the graph owns its reference now
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

    hr = m_pGraph->AddFilter(m_pNullRenderer.Get(), L"Null Renderer");
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS AddFilter(NullRenderer) failed: 0x%08X", hr);
        return false;
    }

    // Software only: no capture filter, device binding or connected pins.
    // Failure is optional; Initialize can still use Intelligent Connect.
    hr = CoCreateInstance(CLSID_AVIDec, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&m_pDecoder));
    if (SUCCEEDED(hr)) {
        hr = m_pGraph->AddFilter(m_pDecoder.Get(), L"AVI Decompressor");
        if (FAILED(hr)) {
            m_pDecoder.Reset();
        }
    }
    if (FAILED(hr)) {
        FACELOGIN_DEBUG(L"DS: optional AVI Decompressor unavailable: 0x%08X", hr);
    }

    return true;
}

bool WebcamCaptureDS::ConnectCapture(IPin* pCaptureOut, IPin* pGrabberIn,
                                      bool preferDecoder) {
    m_directDecoder = false;
    if (m_pDecoder) {
        ComPtr<IPin> decoderIn;
        ComPtr<IPin> decoderOut;
        HRESULT directHr = VFW_E_TYPE_NOT_ACCEPTED;
        if (preferDecoder &&
            GetPin(m_pDecoder.Get(), PINDIR_INPUT, decoderIn) &&
            GetPin(m_pDecoder.Get(), PINDIR_OUTPUT, decoderOut)) {
            directHr = m_pGraph->ConnectDirect(pCaptureOut, decoderIn.Get(), nullptr);
            if (SUCCEEDED(directHr)) {
                directHr = m_pGraph->ConnectDirect(decoderOut.Get(), pGrabberIn, nullptr);
            }
            if (SUCCEEDED(directHr)) {
                m_directDecoder = true;
                return true;
            }
        }

        // Both ends must be disconnected, including the successful first
        // leg when the second fails. Never retry with a partially wired graph.
        bool disconnected = true;
        for (IPin* pin : {pCaptureOut, decoderIn.Get(), decoderOut.Get(), pGrabberIn}) {
            if (pin) {
                const HRESULT hr = m_pGraph->Disconnect(pin);
                if (FAILED(hr)) {
                    FACELOGIN_ERROR(L"DS decoder rollback Disconnect failed: 0x%08X", hr);
                    disconnected = false;
                }
            }
        }
        const HRESULT removeHr = m_pGraph->RemoveFilter(m_pDecoder.Get());
        if (FAILED(removeHr)) {
            FACELOGIN_ERROR(L"DS decoder rollback RemoveFilter failed: 0x%08X", removeHr);
            return false;
        }
        m_pDecoder.Reset();
        if (!disconnected) return false;
        if (preferDecoder) {
            FACELOGIN_DEBUG(L"DS: direct AVI connection unavailable: 0x%08X; "
                            L"using Intelligent Connect", directHr);
        }
    }

    const HRESULT hr = m_pGraph->Connect(pCaptureOut, pGrabberIn);
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS Connect(capture→grabber) failed: 0x%08X", hr);
        return false;
    }
    return true;
}

// Device-dependent half: add the capture filter, negotiate the resolution,
// manually connect  Capture → SampleGrabber → Null Renderer,  and query
// IMediaControl. Data only flows once the caller Run()s the graph.
bool WebcamCaptureDS::AttachCapture(IBaseFilter* pCapture, int width, int height) {
    bool preferDecoder = false;
    HRESULT hr = m_pGraph->AddFilter(pCapture, L"Video Capture");
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS AddFilter(capture) failed: 0x%08X", hr);
        return false;
    }

    // Try to set resolution via IAMStreamConfig
    {
        ComPtr<IPin> captureOut;
        if (GetPin(pCapture, PINDIR_OUTPUT, captureOut)) {
            ComPtr<IAMStreamConfig> config;
            hr = captureOut->QueryInterface(IID_PPV_ARGS(&config));
            if (SUCCEEDED(hr)) {
                AM_MEDIA_TYPE* pmt = nullptr;
                hr = config->GetFormat(&pmt);
                if (SUCCEEDED(hr) && pmt) {
                    // Limit the shortcut to the native format measured in
                    // the A/B. RGB/MJPEG and unknown formats keep legacy wiring.
                    preferDecoder = pmt->subtype == MEDIASUBTYPE_YUY2;
                    if (pmt->formattype == FORMAT_VideoInfo) {
                        auto* pVih = (VIDEOINFOHEADER*)pmt->pbFormat;
                        pVih->bmiHeader.biWidth  = width;
                        pVih->bmiHeader.biHeight = height;
                        pVih->bmiHeader.biSizeImage = width * height * 3;
                        hr = config->SetFormat(pmt);
                        if (SUCCEEDED(hr))
                            FACELOGIN_DEBUG(L"DS: set capture format %dx%d", width, height);
                        else
                            FACELOGIN_WARN(L"DS SetFormat failed: 0x%08X, using default", hr);
                    }
                    FreeMediaType(*pmt);
                    CoTaskMemFree(pmt);
                }
            }
        }
    }

    // Manually connect the graph:
    //    Capture output → SampleGrabber input
    //    SampleGrabber output → Null Renderer input
    // This ensures data flows through the SampleGrabber so BufferCB fires.
    {
        // The skeleton added the grabber under a known name; recover its
        // IBaseFilter from the graph for pin lookup.
        ComPtr<IBaseFilter> grabberFilter;
        hr = m_pGraph->FindFilterByName(L"Sample Grabber",
                                        grabberFilter.GetAddressOf());
        if (FAILED(hr) || !grabberFilter) {
            FACELOGIN_ERROR(L"DS: Sample Grabber filter missing from graph");
            return false;
        }

        ComPtr<IPin> grabberIn;
        ComPtr<IPin> grabberOut;
        ComPtr<IPin> nullIn;
        if (!GetPin(grabberFilter.Get(), PINDIR_INPUT, grabberIn) ||
            !GetPin(grabberFilter.Get(), PINDIR_OUTPUT, grabberOut) ||
            !GetPin(m_pNullRenderer.Get(), PINDIR_INPUT, nullIn)) {
            FACELOGIN_ERROR(L"DS: failed to get pins for graph connection");
            return false;
        }

        ComPtr<IPin> captureOut;
        if (!GetPin(pCapture, PINDIR_OUTPUT, captureOut)) {
            FACELOGIN_ERROR(L"DS: failed to get pins for graph connection");
            return false;
        }

        // Connect capture → grabber
        if (!ConnectCapture(captureOut.Get(), grabberIn.Get(), preferDecoder)) {
            return false;
        }

        // Connect grabber → null renderer
        hr = m_pGraph->Connect(grabberOut.Get(), nullIn.Get());
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
    if (!FindMoniker(devicePath, m_pMoniker)) {
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
    m_pControl.Reset();
    m_pGrabber.Reset();
    m_pNullRenderer.Reset();
    m_pCapture.Reset();
    m_pDecoder.Reset();
    m_pGraph.Reset();
    m_pMoniker.Reset();
    m_preloadDevicePath.clear();
    m_preloadValid = false;
    m_directDecoder = false;
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
        if (BindMoniker(m_pMoniker.Get(), m_pCapture)) {
            // The activation is done; the moniker (a device-registry pointer)
            // is no longer needed while the graph streams.
            m_pMoniker.Reset();
        } else {
            FACELOGIN_WARN(L"DS: preloaded moniker no longer binds — re-enumerating");
            ComPtr<IMoniker> fresh;
            const bool rebound = FindMoniker(devicePath, fresh) &&
                                 BindMoniker(fresh.Get(), m_pCapture);
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
    if (!m_pCapture && !FindCamera(devicePath, m_pCapture)) {
        FACELOGIN_ERROR(L"DS: no camera found — check if camera is connected and "
                        L"drivers are installed.");
        DiscardPreload();
        ShutdownCOM();
        return false;
    }
    if (!AttachCapture(m_pCapture.Get(), m_width, m_height)) {
        FACELOGIN_ERROR(L"DS: failed to build capture graph");
        DiscardPreload();
        ShutdownCOM();
        return false;
    }

    EnterCriticalSection(&m_frameCs);
    m_firstCallbackTicks = 0;
    m_firstBufferReadyTicks = 0;
    m_runReturnedTicks = 0;
    m_startupTimingLogged = false;
    m_runStartedTicks = CaptureTicks();
    LeaveCriticalSection(&m_frameCs);
    HRESULT hr = m_pControl->Run();
    const auto runReturnedAt = CaptureTicks();
    EnterCriticalSection(&m_frameCs);
    m_runReturnedTicks = runReturnedAt;
    LeaveCriticalSection(&m_frameCs);
    if (FAILED(hr)) {
        FACELOGIN_ERROR(L"DS IMediaControl::Run failed: 0x%08X", hr);
        Shutdown();
        return false;
    }

    m_initialized = true;
    FACELOGIN_INFO(L"DirectShow webcam initialized: %dx%d RGB24%s%s",
                   m_width, m_height,
                   m_preloadValid ? L" (preloaded skeleton)" : L"",
                   m_directDecoder ? L" (AVI direct)" : L" (Intelligent Connect)");
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

    const auto grabAt = CaptureTicks();
    EnterCriticalSection(&m_frameCs);

    if (!m_frameReady || !m_frameBuffer || m_frameSize == 0) {
        LeaveCriticalSection(&m_frameCs);
        return false;
    }

    const auto copyStartedAt = CaptureTicks();
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

    const bool logStartup = !m_startupTimingLogged && m_firstBufferReadyTicks != 0;
    const auto copiedAt = logStartup ? CaptureTicks() : 0;
    const auto runStartedAt = m_runStartedTicks;
    const auto runReturnedAt = m_runReturnedTicks;
    const auto callbackAt = m_firstCallbackTicks;
    const auto readyAt = m_firstBufferReadyTicks;
    if (logStartup) m_startupTimingLogged = true;
    LeaveCriticalSection(&m_frameCs);
    if (logStartup) {
        FACELOGIN_INFO(L"DirectShow first frame timing: run_call=%.2f ms, "
                       L"run_to_callback=%.2f ms, callback_copy=%.2f ms, "
                       L"ready_to_grab=%.2f ms, grab_lock_wait=%.2f ms, grab_copy=%.2f ms",
                       CaptureMilliseconds(runStartedAt, runReturnedAt),
                       CaptureMilliseconds(runStartedAt, callbackAt),
                       CaptureMilliseconds(callbackAt, readyAt),
                       CaptureMilliseconds(readyAt, copyStartedAt),
                       CaptureMilliseconds(grabAt, copyStartedAt),
                       CaptureMilliseconds(copyStartedAt, copiedAt));
    }
    return true;
}

// ============================================================================
// Gain controls (face_gain_tune.h)
// ============================================================================

// The procamp interface lives on the capture filter. QI per call — the tune
// touches it a handful of times per graph init, so caching adds state for
// nothing.
bool WebcamCaptureDS::GetGainRange(long* pMin, long* pMax, long* pStep) {
    *pMin = *pMax = *pStep = 0;
    if (!m_pCapture) return false;
    ComPtr<IAMVideoProcAmp> procAmp;
    if (FAILED(m_pCapture->QueryInterface(IID_PPV_ARGS(&procAmp))))
        return false;
    long def = 0, caps = 0;
    return SUCCEEDED(procAmp->GetRange(VideoProcAmp_Gain,
                                       pMin, pMax, pStep, &def, &caps));
}

bool WebcamCaptureDS::GetGain(long* pValue, bool* pManual) {
    *pValue = 0;
    if (pManual) *pManual = false;
    if (!m_pCapture) return false;
    ComPtr<IAMVideoProcAmp> procAmp;
    if (FAILED(m_pCapture->QueryInterface(IID_PPV_ARGS(&procAmp))))
        return false;
    long flags = 0;
    const bool ok = SUCCEEDED(procAmp->Get(VideoProcAmp_Gain, pValue, &flags));
    if (ok && pManual) *pManual = (flags & VideoProcAmp_Flags_Manual) != 0;
    return ok;
}

bool WebcamCaptureDS::SetGain(long value, bool manual) {
    if (!m_pCapture) return false;
    ComPtr<IAMVideoProcAmp> procAmp;
    if (FAILED(m_pCapture->QueryInterface(IID_PPV_ARGS(&procAmp))))
        return false;
    return SUCCEEDED(procAmp->Set(VideoProcAmp_Gain, value,
                                  manual ? VideoProcAmp_Flags_Manual
                                         : VideoProcAmp_Flags_Auto));
}

// Exposure time lives on IAMCameraControl (a separate interface from the
// procamp gain above). Same QI-per-call policy.
bool WebcamCaptureDS::GetExposureRange(long* pMin, long* pMax, long* pStep) {
    *pMin = *pMax = *pStep = 0;
    if (!m_pCapture) return false;
    ComPtr<IAMCameraControl> camCtrl;
    if (FAILED(m_pCapture->QueryInterface(IID_PPV_ARGS(&camCtrl))))
        return false;
    long def = 0, caps = 0;
    return SUCCEEDED(camCtrl->GetRange(CameraControl_Exposure,
                                       pMin, pMax, pStep, &def, &caps));
}

bool WebcamCaptureDS::GetExposure(long* pValue, bool* pManual) {
    *pValue = 0;
    if (pManual) *pManual = false;
    if (!m_pCapture) return false;
    ComPtr<IAMCameraControl> camCtrl;
    if (FAILED(m_pCapture->QueryInterface(IID_PPV_ARGS(&camCtrl))))
        return false;
    long flags = 0;
    const bool ok = SUCCEEDED(camCtrl->Get(CameraControl_Exposure, pValue, &flags));
    if (ok && pManual) *pManual = (flags & CameraControl_Flags_Manual) != 0;
    return ok;
}

bool WebcamCaptureDS::SetExposure(long value, bool manual) {
    if (!m_pCapture) return false;
    ComPtr<IAMCameraControl> camCtrl;
    if (FAILED(m_pCapture->QueryInterface(IID_PPV_ARGS(&camCtrl))))
        return false;
    return SUCCEEDED(camCtrl->Set(CameraControl_Exposure, value,
                                  manual ? CameraControl_Flags_Manual
                                         : CameraControl_Flags_Auto));
}

void WebcamCaptureDS::Shutdown() {
    // Stop streaming first so BufferCB stops firing
    if (m_pControl) {
        m_pControl->Stop();
    }
    m_pControl.Reset();

    // Break the callback reference before releasing the graph. Relying on the
    // filter's final destruction to detach it retained callback/filter state on
    // some camera drivers across repeated auth cycles.
    if (m_pGrabber) {
        m_pGrabber->SetCallback(nullptr, 1);
    }
    m_pGrabber.Reset();
    m_pNullRenderer.Reset();
    m_pCapture.Reset();
    m_pDecoder.Reset();
    m_pGraph.Reset();
    m_pMoniker.Reset();
    m_preloadDevicePath.clear();
    m_preloadValid = false;
    m_directDecoder = false;

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
