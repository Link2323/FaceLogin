// One-off verification tool: confirm that the INT8-quantized w600k_r50
// (QDQ static, per-tensor) loads and embeds identically in the PRODUCTION C++
// onnxruntime (1.23.2) as measured in Python, and time it.
//
// Usage: EmbeddingTest <chip_dir> <model1> [model2 ...]
//   chip_dir: 112x112 BMP chips named like the enrollment photos
//             (front_*, left_*, right_* prefixes group same-angle pairs).
//   Prints, per model: median embed time, same-angle p50/p90/min/max
//   distance, and embedding count.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../../common/frame_image.h"

#include "onnx_models.h"

namespace fs = std::filesystem;

// Minimal uncompressed 24/32-bit BMP reader (replaces dlib::load_bmp, which
// was the last remaining dlib use outside the shipped runtime). The chips
// come from the threshold-calibration pipeline as standard BI_RGB BMPs.
static bool LoadBmp(const std::string& path, facelogin::FrameImage& img) {
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
    if (dibSize < 40) return false;                       // BITMAPINFOHEADER only
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
        const int32_t dstY = topDown ? y : (rows - 1 - y);   // BMP rows are bottom-up
        for (int32_t x = 0; x < w; ++x) {
            const uint8_t* p = row.data() + static_cast<size_t>(x) * (bpp / 8);
            img(dstY, x) = facelogin::RgbPixel(p[2], p[1], p[0]);   // BGR → RGB
        }
    }
    return true;
}

static std::string AngleOf(const std::string& fname) {
    const auto pos = fname.find('_');
    return (pos == std::string::npos) ? fname : fname.substr(0, pos);
}

static double Distance(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        s += d * d;
    }
    return std::sqrt(s);
}

static double Percentile(std::vector<double> vals, double pct) {
    if (vals.empty()) return std::nan("");
    std::sort(vals.begin(), vals.end());
    if (vals.size() == 1) return vals[0];
    const double k = static_cast<double>(vals.size() - 1) * (pct / 100.0);
    const size_t lo = static_cast<size_t>(k);
    const size_t hi = static_cast<size_t>(std::ceil(k));
    if (lo == hi) return vals[lo];
    return vals[lo] + (vals[hi] - vals[lo]) * (k - lo);
}

static std::vector<fs::path> ListChips(const fs::path& dir) {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".bmp") out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: EmbeddingTest <chip_dir> <model1> [model2 ...]\n";
        return 2;
    }
    const fs::path chipDir = argv[1];

    std::vector<facelogin::FrameImage> chips;
    std::vector<std::string> names;
    for (const auto& p : ListChips(chipDir)) {
        facelogin::FrameImage img;
        if (!LoadBmp(p.string(), img)) {
            std::cerr << "[skip] " << p.filename() << " is not a 24/32-bit BI_RGB BMP\n";
            continue;
        }
        if (img.nr() != 112 || img.nc() != 112) {
            std::cerr << "[skip] " << p.filename() << " is " << img.nc()
                      << "x" << img.nr() << ", not 112x112\n";
            continue;
        }
        chips.push_back(std::move(img));
        names.push_back(p.filename().string());
    }
    if (chips.empty()) {
        std::cerr << "no chips loaded from " << chipDir << "\n";
        return 2;
    }
    std::cerr << chips.size() << " chips loaded from " << chipDir << "\n";

    // First model is the reference; group by angle, measure pairwise
    // same-angle distances, and per-chip embed latency.
    std::vector<std::vector<std::vector<float>>> allEmbeddings; // per model
    for (int m = 2; m < argc; ++m) {
        const std::wstring modelPath = [&] {
            const std::string s = argv[m];
            return std::wstring(s.begin(), s.end());
        }();
        facelogin::OnnxRecognizer rec;
        if (!rec.Initialize(modelPath)) {
            std::cerr << "[FAIL] model " << argv[m] << " failed to initialize\n";
            return 1;
        }
        std::cerr << "[OK]   model loaded: " << argv[m] << "\n";

        std::vector<std::vector<float>> embs;
        std::vector<double> times;
        for (size_t i = 0; i < chips.size(); ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            auto emb = rec.ComputeEmbedding(chips[i]);
            const auto t1 = std::chrono::steady_clock::now();
            if (emb.empty()) {
                std::cerr << "[warn] empty embedding for " << names[i] << "\n";
                continue;
            }
            embs.push_back(std::move(emb));
            times.push_back(
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        allEmbeddings.push_back(embs);

        // Same-angle pairs: front-front, left-left, right-right.
        std::vector<double> sameAngle;
        for (const char* angle : {"front", "left", "right"}) {
            std::vector<size_t> idx;
            for (size_t i = 0; i < names.size(); ++i) {
                if (AngleOf(names[i]) == angle) idx.push_back(i);
            }
            for (size_t a = 0; a < idx.size(); ++a) {
                for (size_t b = a + 1; b < idx.size(); ++b) {
                    sameAngle.push_back(
                        Distance(embs[idx[a]], embs[idx[b]]));
                }
            }
        }

        std::sort(times.begin(), times.end());
        std::cout << "\n=== " << argv[m] << " ===\n"
                  << "  embeddings : " << embs.size() << "/" << chips.size()
                  << " (dim " << (embs.empty() ? 0 : embs[0].size()) << ")\n"
                  << "  embed time : median "
                  << Percentile(times, 50) << " ms (min "
                  << Percentile(times, 0) << ", max " << Percentile(times, 100)
                  << ")\n"
                  << "  same-angle : min " << Percentile(sameAngle, 0)
                  << " / p50 " << Percentile(sameAngle, 50)
                  << " / p90 " << Percentile(sameAngle, 90)
                  << " / max " << Percentile(sameAngle, 100)
                  << " (n=" << sameAngle.size() << ")\n";
    }

    // Cross-model: max per-chip embedding delta between model 0 and model 1.
    if (allEmbeddings.size() >= 2) {
        const auto& a = allEmbeddings[0];
        const auto& b = allEmbeddings[1];
        if (a.size() == b.size()) {
            double maxDelta = 0.0;
            for (size_t i = 0; i < a.size(); ++i) {
                const auto d = Distance(a[i], b[i]);
                maxDelta = std::max(maxDelta, d);
            }
            std::cout << "\n=== model[1] vs model[0] ===\n"
                      << "  max embedding distance over "
                      << a.size() << " chips: " << maxDelta << "\n";
        }
    }
    return 0;
}
