#include "onnx_models.h"
#include "webcam_capture.h"
#include "webcam_capture_dshow.h"
#include "logger.h"
#include "config_util.h"
#include "image_utils.h"
#include "registry_util.h"

#include <windows.h>
#include <wincrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using facelogin::OnnxAntiSpoof;
using facelogin::OnnxDetector;
using facelogin::WebcamCapture;
using facelogin::WebcamCaptureDS;
using facelogin::MiniFasEvaluator;

namespace {

struct Options {
    std::wstring label;
    std::wstring split;
    std::wstring subject;
    std::wstring session;
    std::wstring condition;
    fs::path output = L"pad-calibration\\pad_calibration.csv";
    fs::path modelsDir;
    fs::path padModelsDir;
    fs::path dataDir;
    std::wstring cameraDevice;
    std::wstring cameraBackend = L"mf";
    int count = 50;
    int warmupFrames = 10;
    int attemptSize = 5;
    int maxSeconds = 180;
    bool lowLightEnhance = false;
    bool lowLightExplicit = false;
    int cameraRotation = 0;
    bool rotationExplicit = false;
    bool cameraExplicit = false;
    bool validateModelsOnly = false;
    int benchmarkIterations = 0;
};

void PrintUsage() {
    std::wcout
        << L"FaceLogin PAD calibration capture\n\n"
        << L"Usage:\n"
        << L"  PadCalibration.exe --label <live|print|screen> --split <tune|validation>\n"
        << L"      --subject <pseudonym> --session <unique-id> --condition <description>\n"
        << L"      [--data-dir <FaceLogin-data-dir>] [--models <models-dir>]\n"
        << L"      [--pad-models <MiniFASNet-model-dir>]\n"
        << L"      [--output <csv>] [--count 50]\n"
        << L"      [--attempt-size 5] [--warmup 10] [--max-seconds 180]\n"
        << L"      [--camera-device <symbolic-link>] [--rotation <0|90|180|270>]\n"
        << L"      [--backend <mf|ds>]\n"
        << L"      [--low-light-enhance <0|1>]\n\n"
        << L"      [--validate-models-only]\n\n"
        << L"      [--benchmark-iterations <count>] (with --validate-models-only)\n\n"
        << L"Example:\n"
        << L"  PadCalibration.exe --label live --split tune --subject p01 "
           L"--session p01_day1_live_normal --condition normal_front "
           L"--models D:\\FaceLogin\\models --count 50\n";
}

std::optional<std::wstring> NextValue(int& index, int argc, wchar_t* argv[]) {
    if (index + 1 >= argc) return std::nullopt;
    return std::wstring(argv[++index]);
}

bool ParsePositiveInt(const std::wstring& value, int& result) {
    try {
        size_t used = 0;
        const int parsed = std::stoi(value, &used);
        if (used != value.size() || parsed <= 0) return false;
        result = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseArgs(int argc, wchar_t* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return false;
        }

        const auto read = [&]() { return NextValue(i, argc, argv); };
        if (arg == L"--label") {
            auto v = read(); if (!v) return false; options.label = *v;
        } else if (arg == L"--split") {
            auto v = read(); if (!v) return false; options.split = *v;
        } else if (arg == L"--subject") {
            auto v = read(); if (!v) return false; options.subject = *v;
        } else if (arg == L"--session") {
            auto v = read(); if (!v) return false; options.session = *v;
        } else if (arg == L"--condition") {
            auto v = read(); if (!v) return false; options.condition = *v;
        } else if (arg == L"--output") {
            auto v = read(); if (!v) return false; options.output = *v;
        } else if (arg == L"--models") {
            auto v = read(); if (!v) return false; options.modelsDir = *v;
        } else if (arg == L"--pad-models") {
            auto v = read(); if (!v) return false; options.padModelsDir = *v;
        } else if (arg == L"--data-dir") {
            auto v = read(); if (!v) return false; options.dataDir = *v;
        } else if (arg == L"--camera-device") {
            auto v = read(); if (!v) return false; options.cameraDevice = *v;
            options.cameraExplicit = true;
        } else if (arg == L"--backend") {
            auto v = read();
            if (!v || (*v != L"mf" && *v != L"ds")) return false;
            options.cameraBackend = *v;
        } else if (arg == L"--count") {
            auto v = read(); if (!v || !ParsePositiveInt(*v, options.count)) return false;
        } else if (arg == L"--warmup") {
            auto v = read(); if (!v || !ParsePositiveInt(*v, options.warmupFrames)) return false;
        } else if (arg == L"--attempt-size") {
            auto v = read(); if (!v || !ParsePositiveInt(*v, options.attemptSize)) return false;
        } else if (arg == L"--max-seconds") {
            auto v = read(); if (!v || !ParsePositiveInt(*v, options.maxSeconds)) return false;
        } else if (arg == L"--low-light-enhance") {
            auto v = read();
            if (!v || (*v != L"0" && *v != L"1")) return false;
            options.lowLightEnhance = (*v == L"1");
            options.lowLightExplicit = true;
        } else if (arg == L"--rotation") {
            auto v = read();
            if (!v) return false;
            try {
                size_t used = 0;
                options.cameraRotation = std::stoi(*v, &used);
                if (used != v->size() ||
                    (options.cameraRotation != 0 && options.cameraRotation != 90 &&
                     options.cameraRotation != 180 && options.cameraRotation != 270)) {
                    return false;
                }
            } catch (...) {
                return false;
            }
            options.rotationExplicit = true;
        } else if (arg == L"--validate-models-only") {
            options.validateModelsOnly = true;
        } else if (arg == L"--benchmark-iterations") {
            auto v = read();
            if (!v || !ParsePositiveInt(*v, options.benchmarkIterations) ||
                options.benchmarkIterations > 10000) {
                return false;
            }
        } else {
            std::wcerr << L"Unknown argument: " << arg << L"\n";
            return false;
        }
    }

    if (options.benchmarkIterations > 0 && !options.validateModelsOnly) {
        std::wcerr << L"--benchmark-iterations requires --validate-models-only\n";
        return false;
    }

    if (!options.validateModelsOnly) {
        const std::vector<std::wstring> allowedLabels = {
            L"live", L"print", L"screen", L"replay", L"mask"
        };
        if (std::find(allowedLabels.begin(), allowedLabels.end(), options.label) == allowedLabels.end()) {
            std::wcerr << L"--label must be live, print, screen, replay, or mask\n";
            return false;
        }
        if (options.split != L"tune" && options.split != L"validation") {
            std::wcerr << L"--split must be tune or validation\n";
            return false;
        }
        if (options.subject.empty() || options.session.empty() || options.condition.empty()) {
            std::wcerr << L"--subject, --session, and --condition are required\n";
            return false;
        }
        if (options.count < options.attemptSize) {
            std::wcerr << L"--count must be at least --attempt-size\n";
            return false;
        }
    }
    return true;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), chars);
    return result;
}

std::string Csv(const std::wstring& value) {
    const std::string utf8 = WideToUtf8(value);
    std::string escaped;
    escaped.reserve(utf8.size() + 2);
    escaped.push_back('"');
    for (const char ch : utf8) {
        if (ch == '"') escaped.push_back('"');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

std::string UtcNow() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(4) << time.wYear << '-'
        << std::setw(2) << time.wMonth << '-'
        << std::setw(2) << time.wDay << 'T'
        << std::setw(2) << time.wHour << ':'
        << std::setw(2) << time.wMinute << ':'
        << std::setw(2) << time.wSecond << '.'
        << std::setw(3) << time.wMilliseconds << 'Z';
    return out.str();
}

std::uint64_t FrameHash(const dlib::matrix<dlib::rgb_pixel>& frame) {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    // A regular grid is enough to detect an exactly repeated camera buffer and
    // avoids spending more time hashing than running the calibration itself.
    for (long y = 0; y < frame.nr(); y += 4) {
        for (long x = 0; x < frame.nc(); x += 4) {
            const auto& pixel = frame(y, x);
            hash = (hash ^ pixel.red) * prime;
            hash = (hash ^ pixel.green) * prime;
            hash = (hash ^ pixel.blue) * prime;
        }
    }
    hash = (hash ^ static_cast<std::uint64_t>(frame.nr())) * prime;
    hash = (hash ^ static_cast<std::uint64_t>(frame.nc())) * prime;
    return hash;
}

struct PadScores {
    float production;
    float miniV2;
    float miniV1Se;
    float miniEnsemble;
};

constexpr char kMiniV2Sha256[] =
    "b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907";
constexpr char kMiniV1SeSha256[] =
    "ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676";

std::optional<std::string> FileSha256(const fs::path& path) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    const auto cleanup = [&]() {
        if (hash != 0) CryptDestroyHash(hash);
        if (provider != 0) CryptReleaseContext(provider, 0);
    };

    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        cleanup();
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        cleanup();
        return std::nullopt;
    }

    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead > 0 &&
            !CryptHashData(hash, buffer.data(), static_cast<DWORD>(bytesRead), 0)) {
            cleanup();
            return std::nullopt;
        }
    }
    if (input.bad()) {
        cleanup();
        return std::nullopt;
    }

    DWORD digestSize = 0;
    DWORD digestSizeLength = sizeof(digestSize);
    if (!CryptGetHashParam(hash, HP_HASHSIZE,
                           reinterpret_cast<BYTE*>(&digestSize),
                           &digestSizeLength, 0)) {
        cleanup();
        return std::nullopt;
    }
    std::vector<BYTE> digest(digestSize);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestSize, 0)) {
        cleanup();
        return std::nullopt;
    }

    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const BYTE value : digest) {
        encoded << std::setw(2) << static_cast<unsigned int>(value);
    }
    cleanup();
    return encoded.str();
}

bool IsValidScore(float score) {
    return std::isfinite(score) && score >= 0.0f && score <= 1.0f;
}

struct BenchmarkStats {
    double meanMs;
    double p50Ms;
    double p95Ms;
    double minMs;
    double maxMs;
};

template <typename Predictor>
std::optional<BenchmarkStats> BenchmarkPredictor(int iterations, Predictor&& predict) {
    constexpr int kWarmupIterations = 30;
    for (int i = 0; i < kWarmupIterations; ++i) {
        if (!IsValidScore(predict())) return std::nullopt;
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iterations));
    double totalMs = 0.0;
    for (int i = 0; i < iterations; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const float score = predict();
        const auto stopped = std::chrono::steady_clock::now();
        if (!IsValidScore(score)) return std::nullopt;
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            stopped - started).count();
        samples.push_back(elapsedMs);
        totalMs += elapsedMs;
    }

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double fraction) {
        const size_t index = static_cast<size_t>(std::ceil(
            fraction * static_cast<double>(samples.size()))) - 1U;
        return samples[std::min(index, samples.size() - 1U)];
    };
    return BenchmarkStats{
        totalMs / static_cast<double>(samples.size()),
        percentile(0.50),
        percentile(0.95),
        samples.front(),
        samples.back()
    };
}

fs::path FindPadModelsDir(const Options& options, const fs::path& executable) {
    const auto hasBoth = [](const fs::path& directory) {
        return fs::is_regular_file(directory / L"MiniFASNetV2.onnx") &&
               fs::is_regular_file(directory / L"MiniFASNetV1SE.onnx");
    };
    if (!options.padModelsDir.empty()) {
        return hasBoth(options.padModelsDir) ? options.padModelsDir : fs::path{};
    }

    std::vector<fs::path> searchDirectories = {
        options.modelsDir,
        fs::current_path() / L"assets" / L"models"
    };
    fs::path ancestor = fs::absolute(executable).parent_path();
    for (int i = 0; i < 5 && !ancestor.empty(); ++i) {
        searchDirectories.push_back(ancestor / L"assets" / L"models");
        ancestor = ancestor.parent_path();
    }
    for (const auto& directory : searchDirectories) {
        std::error_code ec;
        if (hasBoth(directory)) return fs::weakly_canonical(directory, ec);
    }
    return {};
}

struct CsvWriter {
    explicit CsvWriter(const fs::path& path) : m_path(path) {}

    bool Open() {
        static constexpr const char* header =
            "timestamp_utc,split,label,subject,session,condition,attempt,"
            "frame_in_attempt,accepted_index,camera_frame,status,production_score,"
            "minifas_v2_score,minifas_v1se_score,minifas_ensemble_score,"
            "minifas_v2_sha256,minifas_v1se_sha256,det_score,face_width,face_height,"
            "frame_width,frame_height,low_light_enhance,camera_rotation,"
            "camera_backend,frame_hash";
        std::error_code ec;
        const bool needsHeader = !fs::exists(m_path, ec) || fs::file_size(m_path, ec) == 0;
        if (m_path.has_parent_path()) fs::create_directories(m_path.parent_path(), ec);
        if (!needsHeader) {
            std::ifstream existing(m_path, std::ios::binary);
            std::string firstLine;
            std::getline(existing, firstLine);
            if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
            if (firstLine != header) {
                std::wcerr << L"CSV schema does not match this tool version; use a new output file\n";
                return false;
            }
        }
        m_out.open(m_path, std::ios::binary | std::ios::app);
        if (!m_out) return false;
        if (needsHeader) {
            m_out << header << "\r\n";
        }
        return true;
    }

    void Write(const Options& options, int attempt, int frameInAttempt,
               int acceptedIndex, int cameraFrame, const char* status,
               const PadScores& scores, float detScore,
               float faceWidth, float faceHeight, long frameWidth, long frameHeight,
               std::uint64_t frameHash) {
        m_out << UtcNow() << ','
              << Csv(options.split) << ',' << Csv(options.label) << ','
              << Csv(options.subject) << ',' << Csv(options.session) << ','
              << Csv(options.condition) << ','
              << attempt << ',' << frameInAttempt << ',' << acceptedIndex << ','
              << cameraFrame << ',' << status << ',';
        if (std::isfinite(scores.production))
            m_out << std::fixed << std::setprecision(6) << scores.production;
        m_out << ',';
        if (std::isfinite(scores.miniV2)) m_out << std::fixed << std::setprecision(6) << scores.miniV2;
        m_out << ',';
        if (std::isfinite(scores.miniV1Se)) m_out << std::fixed << std::setprecision(6) << scores.miniV1Se;
        m_out << ',';
        if (std::isfinite(scores.miniEnsemble))
            m_out << std::fixed << std::setprecision(6) << scores.miniEnsemble;
        m_out << ',';
        m_out << kMiniV2Sha256 << ',' << kMiniV1SeSha256 << ',';
        if (std::isfinite(detScore)) m_out << std::fixed << std::setprecision(6) << detScore;
        m_out << ',' << std::fixed << std::setprecision(2)
              << faceWidth << ',' << faceHeight << ',' << frameWidth << ',' << frameHeight << ','
              << (options.lowLightEnhance ? 1 : 0) << ',' << options.cameraRotation
              << ',' << Csv(options.cameraBackend) << ',' << frameHash << "\r\n";
        m_out.flush();
    }

private:
    fs::path m_path;
    std::ofstream m_out;
};

class ComScope {
public:
    ComScope() : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(m_hr)) CoUninitialize(); }
    bool Ready() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
private:
    HRESULT m_hr;
};

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && (std::wstring(argv[1]) == L"--help" ||
                      std::wstring(argv[1]) == L"-h")) {
        PrintUsage();
        return 0;
    }
    Options options;
    if (!ParseArgs(argc, argv, options)) {
        if (argc <= 1) PrintUsage();
        return argc <= 1 ? 2 : 1;
    }

    if (options.dataDir.empty()) {
        std::wstring defaultData = ReadRegString(REGVAL_DATA_PATH, L"");
        if (defaultData.empty()) {
            wchar_t programData[MAX_PATH] = {};
            const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
            defaultData = (length > 0 && length < MAX_PATH)
                ? std::wstring(programData) + L"\\FaceLogin"
                : L"C:\\ProgramData\\FaceLogin";
        }
        options.dataDir = defaultData;
    }
    if (options.modelsDir.empty()) options.modelsDir = options.dataDir / L"models";

    facelogin::Logger::Instance().SetMinLevel(facelogin::LogLevel::Warning);
    const facelogin::AppConfig activeConfig = facelogin::LoadConfig(options.dataDir.wstring());
    if (!options.lowLightExplicit) options.lowLightEnhance = activeConfig.low_light_enhance;
    if (!options.rotationExplicit) options.cameraRotation = activeConfig.camera_rotation;
    if (!options.cameraExplicit) options.cameraDevice = Utf8ToWide(activeConfig.camera_device);

    const fs::path detectorPath = options.modelsDir / L"det_10g_gnkps.onnx";
    if (!fs::is_regular_file(detectorPath)) {
        std::wcerr << L"Required models not found in " << options.modelsDir << L"\n"
                   << L"Expected: det_10g_gnkps.onnx\n";
        return 3;
    }

    const fs::path padDir = FindPadModelsDir(options, argv[0]);
    if (padDir.empty()) {
        std::wcerr << L"PAD model directory must contain MiniFASNetV2.onnx and "
                      L"MiniFASNetV1SE.onnx\n";
        return 3;
    }

    ComScope com;
    if (!com.Ready()) {
        std::wcerr << L"COM initialization failed\n";
        return 5;
    }

    OnnxDetector detector;
    OnnxAntiSpoof pad;
    MiniFasEvaluator miniV2;
    MiniFasEvaluator miniV1Se;
    if (!detector.Initialize(detectorPath.wstring())) {
        std::wcerr << L"Failed to initialize SCRFD detector\n";
        return 6;
    }
    const fs::path v2Path = padDir / L"MiniFASNetV2.onnx";
    const fs::path v1SePath = padDir / L"MiniFASNetV1SE.onnx";
    if (fs::file_size(v2Path) != 1743581 || fs::file_size(v1SePath) != 1742335) {
        std::wcerr << L"PAD model size mismatch; run scripts\\download_minifas_models.ps1\n";
        return 7;
    }
    const auto v2Hash = FileSha256(v2Path);
    const auto v1SeHash = FileSha256(v1SePath);
    if (!v2Hash || !v1SeHash || *v2Hash != kMiniV2Sha256 ||
        *v1SeHash != kMiniV1SeSha256) {
        std::wcerr << L"PAD model SHA-256 mismatch; run "
                      L"scripts\\download_minifas_models.ps1\n";
        return 7;
    }
    if (!miniV2.Initialize(v2Path.wstring(), 2.7f) ||
        !miniV1Se.Initialize(v1SePath.wstring(), 4.0f)) {
        std::wcerr << L"Failed to initialize MiniFASNet diagnostic models\n";
        return 7;
    }
    if (!pad.Initialize(v2Path.wstring(), v1SePath.wstring())) {
        std::wcerr << L"Failed to initialize production dual MiniFAS PAD\n";
        return 7;
    }

    if (options.validateModelsOnly) {
        dlib::matrix<dlib::rgb_pixel> synthetic(480, 640);
        for (long y = 0; y < synthetic.nr(); ++y) {
            for (long x = 0; x < synthetic.nc(); ++x) {
                synthetic(y, x) = dlib::rgb_pixel(
                    static_cast<unsigned char>((x + y) % 256),
                    static_cast<unsigned char>((x * 2 + y) % 256),
                    static_cast<unsigned char>((x + y * 2) % 256));
            }
        }
        const dlib::rectangle testRect(220, 90, 420, 390);
        const float productionScore = pad.Predict(synthetic, testRect);
        const float v2Score = miniV2.Predict(synthetic, testRect);
        const float v1SeScore = miniV1Se.Predict(synthetic, testRect);
        if (!IsValidScore(productionScore) || !IsValidScore(v2Score) ||
            !IsValidScore(v1SeScore)) {
            std::wcerr << L"Model graph loaded, but synthetic inference returned an invalid score\n";
            return 11;
        }
        std::wcout << L"Model validation and synthetic inference OK: dual MiniFAS="
                   << productionScore << L", MiniFASNetV2=" << v2Score
                   << L", MiniFASNetV1SE=" << v1SeScore << L"\n";
        if (options.benchmarkIterations > 0) {
            const auto productionStats = BenchmarkPredictor(
                options.benchmarkIterations,
                [&]() { return pad.Predict(synthetic, testRect); });
            const auto v2Stats = BenchmarkPredictor(
                options.benchmarkIterations,
                [&]() { return miniV2.Predict(synthetic, testRect); });
            const auto v1SeStats = BenchmarkPredictor(
                options.benchmarkIterations,
                [&]() { return miniV1Se.Predict(synthetic, testRect); });
            if (!productionStats || !v2Stats || !v1SeStats) {
                std::wcerr << L"Benchmark inference returned an invalid score\n";
                return 12;
            }
            const auto report = [&](const wchar_t* name, const BenchmarkStats& stats) {
                std::wcout << std::left << std::setw(24) << name << std::right
                           << L" mean=" << std::fixed << std::setprecision(3) << stats.meanMs
                           << L" ms, p50=" << stats.p50Ms
                           << L" ms, p95=" << stats.p95Ms
                           << L" ms, min=" << stats.minMs
                           << L" ms, max=" << stats.maxMs << L" ms\n";
            };
            std::wcout << L"CPU benchmark (" << options.benchmarkIterations
                       << L" iterations after 30 warm-ups; crop + preprocess + ORT, "
                          L"detector excluded):\n";
            report(L"V2+V1SE production", *productionStats);
            report(L"MiniFASNetV2", *v2Stats);
            report(L"MiniFASNetV1SE", *v1SeStats);
        }
        return 0;
    }

    CsvWriter csv(options.output);
    if (!csv.Open()) {
        std::wcerr << L"Cannot open output CSV: " << options.output << L"\n";
        return 4;
    }

    WebcamCapture cameraMf;
    WebcamCaptureDS cameraDs;
    const auto initializeCamera = [&]() {
        return options.cameraBackend == L"ds"
            ? cameraDs.Initialize(1280, 720, options.cameraDevice)
            : cameraMf.Initialize(1280, 720, options.cameraDevice);
    };
    const auto grabFrame = [&](dlib::matrix<dlib::rgb_pixel>& output) {
        return options.cameraBackend == L"ds"
            ? cameraDs.GrabFrame(output)
            : cameraMf.GrabFrame(output);
    };
    const auto shutdownCamera = [&]() {
        if (options.cameraBackend == L"ds") cameraDs.Shutdown();
        else cameraMf.Shutdown();
    };
    if (!initializeCamera()) {
        std::wcerr << L"Failed to open camera. Close FaceLoginConsole and other camera apps first.\n";
        return 8;
    }

    std::wcout << L"Session: " << options.session << L"\n"
               << L"Label: " << options.label << L", split: " << options.split << L"\n"
               << L"Condition: " << options.condition << L"\n"
               << L"Production config: rotation=" << options.cameraRotation
               << L", low-light-enhance=" << (options.lowLightEnhance ? L"on" : L"off") << L"\n"
               << L"Camera backend: " << options.cameraBackend << L"\n"
               << L"PAD models: " << padDir.wstring() << L"\n"
               << L"Output: " << options.output << L"\n"
               << L"Keep exactly one face in view. Starting warm-up...\n";

    dlib::matrix<dlib::rgb_pixel> frame;
    int warmed = 0;
    const auto warmupDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(std::min(options.maxSeconds, 30));
    while (warmed < options.warmupFrames) {
        if (grabFrame(frame)) {
            ++warmed;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (std::chrono::steady_clock::now() >= warmupDeadline) {
            std::wcerr << L"Camera warm-up timed out before a frame was available\n";
            shutdownCamera();
            return 9;
        }
    }

    std::wcout << L"Capturing " << options.count << L" detected, unique face frames.\n";
    const auto started = std::chrono::steady_clock::now();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const PadScores emptyScores{nan, nan, nan, nan};
    int accepted = 0;
    int cameraFrame = 0;
    int noFace = 0;
    int invalid = 0;
    int duplicates = 0;
    int grabFailures = 0;
    std::uint64_t lastHash = 0;
    bool haveHash = false;
    double scoreSum = 0.0;
    float scoreMin = 1.0f;
    float scoreMax = 0.0f;
    int productionScoreCount = 0;

    while (accepted < options.count) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed >= options.maxSeconds) break;

        if (!grabFrame(frame)) {
            ++grabFailures;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        ++cameraFrame;
        facelogin::RotateFrame(frame, options.cameraRotation);
        const std::uint64_t hash = FrameHash(frame);
        if (haveHash && hash == lastHash) {
            ++duplicates;
            csv.Write(options, accepted / options.attemptSize,
                      accepted % options.attemptSize, accepted, cameraFrame,
                      "duplicate", emptyScores, nan, 0.0f, 0.0f,
                      frame.nc(), frame.nr(), hash);
            continue;
        }
        lastHash = hash;
        haveHash = true;

        const auto detection = detector.DetectLargestFace(frame);
        if (!detection) {
            ++noFace;
            csv.Write(options, accepted / options.attemptSize,
                      accepted % options.attemptSize, accepted, cameraFrame,
                      "no_face", emptyScores, nan, 0.0f, 0.0f,
                      frame.nc(), frame.nr(), hash);
            continue;
        }

        const dlib::rectangle rect(
            static_cast<long>(detection->x1), static_cast<long>(detection->y1),
            static_cast<long>(detection->x2), static_cast<long>(detection->y2));
        PadScores scores{pad.Predict(frame, rect), nan, nan, nan};
        scores.miniV2 = miniV2.Predict(frame, rect);
        scores.miniV1Se = miniV1Se.Predict(frame, rect);
        if (IsValidScore(scores.miniV2) && IsValidScore(scores.miniV1Se)) {
            // The production score is the arithmetic mean of the same class-1
            // probabilities. Keeping it here makes CSV diagnostics auditable.
            scores.miniEnsemble = (scores.miniV2 + scores.miniV1Se) / 2.0f;
        }
        const float faceWidth = detection->x2 - detection->x1;
        const float faceHeight = detection->y2 - detection->y1;
        const bool productionValid = IsValidScore(scores.production);
        const bool diagnosticScoresValid = IsValidScore(scores.miniV2) &&
            IsValidScore(scores.miniV1Se) && IsValidScore(scores.miniEnsemble);
        const bool anyDiagnosticValid = IsValidScore(scores.miniV2) ||
            IsValidScore(scores.miniV1Se) || IsValidScore(scores.miniEnsemble);
        const bool allScoresValid = productionValid && diagnosticScoresValid;
        const char* status = allScoresValid ? "valid" :
            (productionValid || anyDiagnosticValid ? "partial_score" : "invalid_score");
        if (!allScoresValid) ++invalid;

        const int attempt = accepted / options.attemptSize;
        const int frameInAttempt = accepted % options.attemptSize;
        csv.Write(options, attempt, frameInAttempt, accepted, cameraFrame,
                  status, scores, detection->score, faceWidth,
                  faceHeight, frame.nc(), frame.nr(), hash);
        ++accepted;
        if (productionValid) {
            ++productionScoreCount;
            scoreSum += scores.production;
            scoreMin = std::min(scoreMin, scores.production);
            scoreMax = std::max(scoreMax, scores.production);
        }
        std::wcout << L"[" << accepted << L"/" << options.count << L"] score=";
        if (productionValid) {
            std::wcout << std::fixed << std::setprecision(4) << scores.production;
        } else {
            std::wcout << L"error";
        }
        std::wcout << L" v2=";
        if (IsValidScore(scores.miniV2)) std::wcout << scores.miniV2; else std::wcout << L"error";
        std::wcout << L" v1se=";
        if (IsValidScore(scores.miniV1Se)) std::wcout << scores.miniV1Se; else std::wcout << L"error";
        std::wcout << L" ensemble=";
        if (IsValidScore(scores.miniEnsemble)) std::wcout << scores.miniEnsemble; else std::wcout << L"error";
        std::wcout
                   << L" det=" << std::setprecision(3) << detection->score
                   << L" attempt=" << attempt + 1 << L"\n";
    }

    shutdownCamera();
    std::wcout << L"\nCaptured " << accepted << L" detected face frames; no-face=" << noFace
               << L", duplicate=" << duplicates << L", invalid=" << invalid
               << L", grab-failure=" << grabFailures << L".\n";
    if (productionScoreCount > 0) {
        std::wcout << L"Score min/mean/max: " << std::fixed << std::setprecision(4)
                   << scoreMin << L" / " << scoreSum / productionScoreCount << L" / " << scoreMax << L"\n";
    }
    if (accepted < options.count) {
        std::wcerr << L"Capture stopped before the target count (timeout). Re-run with a new session id.\n";
        return 9;
    }
    return 0;
}
