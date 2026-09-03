// Offline PAD scorer for still images: runs the same production scoring path
// (SCRFD largest-face detection -> dual MiniFAS 50/50 fusion) as FaceLogin's
// auth pipeline, but on BMP files instead of a live camera. Used to explain
// lock-screen liveness failures from photos taken at the failing position.
//
// Usage: PadScore.exe <models_dir> <bmp> [<bmp> ...]
//   Prints one CSV line per image:
//   file,status,production,v2,v1se,det_score,face_width,face_height,frame_w,frame_h
#include "onnx_models.h"
#include "frame_image.h"

#include <windows.h>
#include <wincrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using facelogin::OnnxAntiSpoof;
using facelogin::OnnxDetector;
using facelogin::MiniFasEvaluator;
using facelogin::FrameImage;
using facelogin::FaceRect;
using facelogin::RgbPixel;

namespace {

// 24/32-bit BI_RGB BMP reader, identical to tools/embedding_test.
bool LoadBmp(const std::string& path, FrameImage& img) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint8_t hdr[54];
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (f.gcount() != static_cast<std::streamsize>(sizeof(hdr)) ||
        hdr[0] != 'B' || hdr[1] != 'M') {
        return false;
    }
    auto u32 = [&](int o) { return static_cast<uint32_t>(hdr[o]) |
                                   (static_cast<uint32_t>(hdr[o + 1]) << 8) |
                                   (static_cast<uint32_t>(hdr[o + 2]) << 16) |
                                   (static_cast<uint32_t>(hdr[o + 3]) << 24); };
    auto i16 = [&](int o) { return static_cast<int16_t>(hdr[o] | (hdr[o + 1] << 8)); };
    auto i32 = [&](int o) { return static_cast<int32_t>(u32(o)); };

    const uint32_t dataOffset = u32(10);
    const uint32_t dibSize = u32(14);
    if (dibSize < 40) return false;
    const int32_t w = i32(18), h = i32(22);
    const int16_t bpp = i16(28);
    const uint32_t compression = u32(30);
    if (w <= 0 || h == 0 || (bpp != 24 && bpp != 32) || compression != 0) {
        return false;
    }
    const bool topDown = h < 0;
    const int32_t rows = std::abs(h);
    img.set_size(rows, w);

    const size_t rowSize = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
    std::vector<uint8_t> row(rowSize);
    f.seekg(dataOffset);
    for (int32_t y = 0; y < rows; ++y) {
        f.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(rowSize));
        if (!f) return false;
        const int32_t dstY = topDown ? y : (rows - 1 - y);
        for (int32_t x = 0; x < w; ++x) {
            const uint8_t* p = row.data() + static_cast<size_t>(x) * (bpp / 8);
            img(dstY, x) = RgbPixel(p[2], p[1], p[0]);
        }
    }
    return true;
}

class ComScope {
public:
    ComScope() : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(m_hr)) CoUninitialize(); }
    bool Ready() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
private:
    HRESULT m_hr;
};

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: PadScore.exe <models_dir> <bmp> [<bmp> ...]\n";
        return 2;
    }
    const fs::path modelsDir = fs::path(argv[1]);
    const fs::path detectorPath = modelsDir / L"det_10g_gnkps.onnx";
    const fs::path v2Path = modelsDir / L"MiniFASNetV2.onnx";
    const fs::path v1SePath = modelsDir / L"MiniFASNetV1SE.onnx";
    for (const auto& p : {detectorPath, v2Path, v1SePath}) {
        if (!fs::is_regular_file(p)) {
            std::wcerr << L"Missing model: " << p.wstring() << L"\n";
            return 3;
        }
    }

    ComScope com;
    if (!com.Ready()) {
        std::cerr << "COM initialization failed\n";
        return 5;
    }

    OnnxDetector detector;
    OnnxAntiSpoof pad;
    MiniFasEvaluator miniV2;
    MiniFasEvaluator miniV1Se;
    if (!detector.Initialize(detectorPath.wstring())) {
        std::cerr << "Failed to initialize SCRFD detector\n";
        return 6;
    }
    if (!miniV2.Initialize(v2Path.wstring(), 2.7f) ||
        !miniV1Se.Initialize(v1SePath.wstring(), 4.0f) ||
        !pad.Initialize(v2Path.wstring(), v1SePath.wstring())) {
        std::cerr << "Failed to initialize MiniFAS models\n";
        return 7;
    }

    std::cout << "file,status,production,v2,v1se,det_score,face_width,face_height,frame_w,frame_h\n";
    for (int i = 2; i < argc; ++i) {
        const fs::path path(argv[i]);
        FrameImage frame;
        if (!LoadBmp(path.string(), frame)) {
            std::cout << path.filename().string() << ",load_error,,,,,,,,\n";
            continue;
        }
        const auto detection = detector.DetectLargestFace(frame);
        if (!detection) {
            std::cout << path.filename().string() << ",no_face,,,,,,,,\n";
            continue;
        }
        const FaceRect rect(static_cast<long>(detection->x1),
                            static_cast<long>(detection->y1),
                            static_cast<long>(detection->x2),
                            static_cast<long>(detection->y2));
        const float production = pad.Predict(frame, rect);
        const float v2 = miniV2.Predict(frame, rect);
        const float v1Se = miniV1Se.Predict(frame, rect);
        std::cout << path.filename().string()
                  << ",valid," << std::fixed << std::setprecision(4)
                  << production << ',' << v2 << ',' << v1Se << ','
                  << detection->score << ','
                  << (detection->x2 - detection->x1) << ','
                  << (detection->y2 - detection->y1) << ','
                  << frame.nc() << ',' << frame.nr() << '\n';
    }
    return 0;
}
