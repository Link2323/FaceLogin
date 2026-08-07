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
#include <iostream>
#include <string>
#include <vector>

#include <dlib/image_io.h>
#include <dlib/matrix.h>
#include <dlib/pixel.h>

#include "onnx_models.h"

namespace fs = std::filesystem;

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

    std::vector<dlib::matrix<dlib::rgb_pixel>> chips;
    std::vector<std::string> names;
    for (const auto& p : ListChips(chipDir)) {
        dlib::matrix<dlib::rgb_pixel> img;
        dlib::load_bmp(img, p.string());
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
