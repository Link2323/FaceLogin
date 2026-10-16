#include "preview_frame_encoder.h"
#include "../common/logger.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <limits>
#include <vector>

namespace facelogin {
namespace {
using Microsoft::WRL::ComPtr;

bool Failed(HRESULT result, const wchar_t* operation) {
    if (SUCCEEDED(result)) return false;
    FACELOGIN_ERROR(L"Preview JPEG %s failed: 0x%08X", operation, result);
    return true;
}

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

} // namespace

PreviewFrameEncoder::PreviewFrameEncoder() {
    Failed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&m_factory)), L"factory creation");
}

PreviewFrameEncoder::~PreviewFrameEncoder() {
    if (m_factory) m_factory->Release();
}

bool PreviewFrameEncoder::Encode(const FrameImage& frame, std::string& out) {
    out.clear();
    if (!m_factory || frame.size() == 0) return false;
    const size_t width = frame.nc();
    const size_t height = frame.nr();
    const size_t maxSize = std::numeric_limits<UINT>::max();
    if (width == 0 || height == 0 || width > maxSize / 4 || height > maxSize / (width * 4)) {
        FACELOGIN_ERROR(L"Preview JPEG dimensions exceed WIC buffer limits");
        return false;
    }
    const UINT srcW = static_cast<UINT>(width);
    const UINT srcH = static_cast<UINT>(height);
    const UINT stride = srcW * 4;
    std::vector<BYTE> bgra(static_cast<size_t>(stride) * srcH);
    for (UINT y = 0; y < srcH; ++y) {
        BYTE* row = bgra.data() + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < srcW; ++x) {
            const auto& pixel = frame(y, x);
            row[x * 4] = pixel.blue;
            row[x * 4 + 1] = pixel.green;
            row[x * 4 + 2] = pixel.red;
            row[x * 4 + 3] = 255;
        }
    }

    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapFrameEncode> encodedFrame;
    ComPtr<IPropertyBag2> properties;
    if (Failed(m_factory->CreateBitmapFromMemory(srcW, srcH, GUID_WICPixelFormat32bppBGR,
                stride, static_cast<UINT>(bgra.size()), bgra.data(), bitmap.GetAddressOf()), L"bitmap creation") ||
        Failed(m_factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                encoder.GetAddressOf()), L"encoder creation") ||
        Failed(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()), L"stream creation") ||
        Failed(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), L"encoder initialization") ||
        Failed(encoder->CreateNewFrame(encodedFrame.GetAddressOf(), properties.GetAddressOf()),
                L"frame creation")) return false;
    if (!properties) {
        FACELOGIN_ERROR(L"Preview JPEG quality properties unavailable");
        return false;
    }
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT quality{};
    quality.vt = VT_R4;
    quality.fltVal = 0.70f;
    const HRESULT qualityResult = properties->Write(1, &option, &quality);
    VariantClear(&quality);
    if (Failed(qualityResult, L"quality setting") ||
        Failed(encodedFrame->Initialize(properties.Get()), L"frame initialization") ||
        Failed(encodedFrame->SetSize(srcW, srcH), L"size setting") ||
        Failed(encodedFrame->WriteSource(bitmap.Get(), nullptr), L"pixel writing") ||
        Failed(encodedFrame->Commit(), L"frame commit") ||
        Failed(encoder->Commit(), L"encoder commit")) return false;

    STATSTG stat{};
    if (Failed(stream->Stat(&stat, STATFLAG_NONAME), L"stream size")) return false;
    if (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > std::numeric_limits<ULONG>::max()) {
        FACELOGIN_ERROR(L"Preview JPEG invalid stream size");
        return false;
    }
    const ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
    std::vector<BYTE> jpeg(size);
    LARGE_INTEGER beginning{};
    ULONG read = 0;
    if (Failed(stream->Seek(beginning, STREAM_SEEK_SET, nullptr), L"stream seek") ||
        Failed(stream->Read(jpeg.data(), size, &read), L"stream read")) return false;
    if (read != size) {
        FACELOGIN_ERROR(L"Preview JPEG incomplete stream read");
        return false;
    }
    out = "data:image/jpeg;base64," + EncodeBase64(jpeg.data(), jpeg.size());
    return true;
}

} // namespace facelogin
