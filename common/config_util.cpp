#include "config_util.h"
#include "registry_util.h"
#include "logger.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace facelogin {

static std::wstring ResolveDataDir(const std::wstring& dataDir) {
    if (!dataDir.empty()) return dataDir;
    auto fromReg = ReadRegString(REGVAL_DATA_PATH, L"");
    if (!fromReg.empty()) return fromReg;
    wchar_t programData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, programData)))
        return std::wstring(programData) + L"\\FaceLogin";
    return L"C:\\ProgramData\\FaceLogin";
}

// Simple JSON writer — flat struct, no nested objects
static void jsonWriteString(std::ostringstream& ss, const std::string& s) {
    ss << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') ss << '\\';
        ss << c;
    }
    ss << '"';
}

// Minimal JSON parser for flat objects. Returns empty string on error.
// Handles \" , \\ and \uXXXX escapes (backslash-heavy values like camera
// device symbolic links would otherwise get corrupted by the round-trip;
// \uXXXX is emitted by JSON.stringify for characters like '&' in WebView2).
static std::string jsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    // skip ':'
    pos++;
    // skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] != '"') {
        // Number or literal
        auto end = json.find_first_of(",}\n\r \t", pos);
        if (end == std::string::npos) return json.substr(pos);
        return json.substr(pos, end - pos);
    }

    // Parse the quoted string, unescaping \\ , \" and \uXXXX as we go.
    pos++;  // skip opening quote
    std::string out;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') break;               // closing quote
        if (c == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            if (next == '\\' || next == '"') {
                out.push_back(next);       // \\ → \  and  \" → "
                pos += 2;
                continue;
            }
            if (next == 'u' && pos + 5 < json.size()) {
                // \uXXXX — parse 4 hex digits into a UTF-8 char.
                bool ok = true;
                unsigned int code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = json[pos + 2 + i];
                    int v = -1;
                    if (h >= '0' && h <= '9') v = h - '0';
                    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                    if (v < 0) { ok = false; break; }
                    code = (code << 4) | static_cast<unsigned int>(v);
                }
                if (ok) {
                    // Encode the code point as UTF-8.
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    pos += 6;
                    continue;
                }
            }
            // Unknown escape: keep the backslash literally.
            out.push_back(c);
            pos++;
            continue;
        }
        out.push_back(c);
        pos++;
    }
    return out;
}

static float jsonGetFloat(const std::string& json, const std::string& key, float defVal) {
    auto s = jsonGetString(json, key);
    if (s.empty()) return defVal;
    try { return std::stof(s); } catch (...) { return defVal; }
}

static int jsonGetInt(const std::string& json, const std::string& key, int defVal) {
    auto s = jsonGetString(json, key);
    if (s.empty()) return defVal;
    try { return std::stoi(s); } catch (...) { return defVal; }
}

std::string ConfigToJson(const AppConfig& cfg) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  "; jsonWriteString(ss, "match_threshold"); ss << ": " << cfg.match_threshold << ",\n";
    ss << "  "; jsonWriteString(ss, "anti_spoof_threshold"); ss << ": " << cfg.anti_spoof_threshold << ",\n";
    ss << "  "; jsonWriteString(ss, "low_light_enhance"); ss << ": " << (cfg.low_light_enhance ? "true" : "false") << ",\n";
    ss << "  "; jsonWriteString(ss, "camera_rotation"); ss << ": " << cfg.camera_rotation << ",\n";
    ss << "  "; jsonWriteString(ss, "camera_device"); ss << ": "; jsonWriteString(ss, cfg.camera_device); ss << ",\n";
    ss << "  "; jsonWriteString(ss, "ema_learning"); ss << ": " << (cfg.ema_learning ? "true" : "false") << ",\n";
    ss << "  "; jsonWriteString(ss, "failure_learning"); ss << ": " << (cfg.failure_learning ? "true" : "false") << ",\n";
    ss << "  "; jsonWriteString(ss, "learning_alpha"); ss << ": " << cfg.learning_alpha << ",\n";
    ss << "  "; jsonWriteString(ss, "learning_distance_gate"); ss << ": " << cfg.learning_distance_gate << ",\n";
    ss << "  "; jsonWriteString(ss, "learning_norm_floor"); ss << ": " << cfg.learning_norm_floor << ",\n";
    ss << "  "; jsonWriteString(ss, "learning_min_interval_sec"); ss << ": " << cfg.learning_min_interval_sec << "\n";
    ss << "}\n";
    return ss.str();
}

AppConfig ConfigFromJson(const std::string& json) {
    AppConfig cfg = DefaultConfig();
    cfg.match_threshold = jsonGetFloat(json, "match_threshold", 0.80f);
    // UI permits [0.70, 1.00] for 512-D (calibrated band; see
    // docs/threshold-calibration.md). Enforce here too because config.json
    // can be hand-edited. EmbeddingThresholdForDim re-clamps at match time,
    // but bounding at load keeps the persisted value honest and the log clean.
    if (!std::isfinite(cfg.match_threshold) ||
        cfg.match_threshold < 0.70f || cfg.match_threshold > 1.00f) {
        FACELOGIN_WARN(L"Unsafe match_threshold=%.3f; enforcing calibrated default 0.80",
                       cfg.match_threshold);
        cfg.match_threshold = 0.80f;
    }
    cfg.anti_spoof_threshold = jsonGetFloat(json, "anti_spoof_threshold", 0.28f);
    // The UI only permits [0.15, 0.50]. Enforce the same range in the
    // security boundary because config.json can also be edited by hand.
    // In particular, a negative threshold would make Predict()'s -1 error
    // sentinel pass the comparison and turn an inference failure into success.
    if (!std::isfinite(cfg.anti_spoof_threshold) ||
        cfg.anti_spoof_threshold < 0.15f || cfg.anti_spoof_threshold > 0.50f) {
        FACELOGIN_WARN(L"Unsafe anti_spoof_threshold=%.3f; enforcing default 0.28",
                       cfg.anti_spoof_threshold);
        cfg.anti_spoof_threshold = 0.28f;
    }
    // Missing key adopts the current default (true); an explicit value is
    // honored as written. Configs persisted by older builds carry an explicit
    // false and keep it — the loader must not silently flip user files.
    const std::string enhance = jsonGetString(json, "low_light_enhance");
    cfg.low_light_enhance = enhance.empty() ? cfg.low_light_enhance
                                            : (enhance == "true");
    int rotation = jsonGetInt(json, "camera_rotation", 0);
    // Only accept 0/90/180/270; anything else silently does nothing in
    // RotateFrame, so fall back to 0 and log it — a configured-but-ignored
    // rotation is a silent failure the user can't diagnose.
    if (rotation != 90 && rotation != 180 && rotation != 270) {
        if (rotation != 0)
            FACELOGIN_WARN(L"Invalid camera_rotation=%d in config, falling back to 0", rotation);
        rotation = 0;
    }
    cfg.camera_rotation = rotation;

    // ---- Template learning gates (docs/progressive-learning-v2.md §1/§3).
    // Missing keys adopt the calibrated defaults; hand-edited values outside
    // the safe band snap back — the distance gate in particular must stay
    // far below match_threshold or an impostor could poison templates.
    // Red line 7 (2026-09-04): independent ema_learning/failure_learning
    // switches. The legacy progressive_learning key is honored when
    // ema_learning is absent so a disabled setup survives the rename.
    const std::string emaLearning = jsonGetString(json, "ema_learning");
    if (!emaLearning.empty()) {
        cfg.ema_learning = (emaLearning == "true");
    } else {
        const std::string legacyLearning = jsonGetString(json, "progressive_learning");
        if (!legacyLearning.empty()) cfg.ema_learning = (legacyLearning == "true");
    }
    const std::string failureLearning = jsonGetString(json, "failure_learning");
    if (!failureLearning.empty()) cfg.failure_learning = (failureLearning == "true");
    cfg.learning_alpha = jsonGetFloat(json, "learning_alpha", 0.10f);
    if (!std::isfinite(cfg.learning_alpha) ||
        cfg.learning_alpha < 0.05f || cfg.learning_alpha > 0.15f) {
        FACELOGIN_WARN(L"Unsafe learning_alpha=%.3f; enforcing 0.10",
                       cfg.learning_alpha);
        cfg.learning_alpha = 0.10f;
    }
    cfg.learning_distance_gate = jsonGetFloat(json, "learning_distance_gate", 0.65f);
    if (!std::isfinite(cfg.learning_distance_gate) ||
        cfg.learning_distance_gate < 0.35f || cfg.learning_distance_gate > 0.65f) {
        FACELOGIN_WARN(L"Unsafe learning_distance_gate=%.3f; enforcing 0.65",
                       cfg.learning_distance_gate);
        cfg.learning_distance_gate = 0.65f;
    }
    cfg.learning_norm_floor = jsonGetFloat(json, "learning_norm_floor", 19.1f);
    if (!std::isfinite(cfg.learning_norm_floor) ||
        cfg.learning_norm_floor < 15.0f || cfg.learning_norm_floor > 25.0f) {
        FACELOGIN_WARN(L"Unsafe learning_norm_floor=%.3f; enforcing 19.1",
                       cfg.learning_norm_floor);
        cfg.learning_norm_floor = 19.1f;
    }
    cfg.learning_min_interval_sec = jsonGetInt(json, "learning_min_interval_sec", 60);
    if (cfg.learning_min_interval_sec < 10 || cfg.learning_min_interval_sec > 3600) {
        FACELOGIN_WARN(L"Unsafe learning_min_interval_sec=%d; enforcing 60",
                       cfg.learning_min_interval_sec);
        cfg.learning_min_interval_sec = 60;
    }
    cfg.camera_rotation = rotation;
    auto cam = jsonGetString(json, "camera_device");
    if (!cam.empty()) cfg.camera_device = cam;
    return cfg;
}

AppConfig DefaultConfig() {
    return AppConfig{};
}

AppConfig LoadConfig(const std::wstring& dataDir) {
    std::wstring dir = ResolveDataDir(dataDir);
    std::wstring dataSubDir = dir + L"\\data";
    CreateDirectoryW(dataSubDir.c_str(), nullptr);
    std::wstring path = dataSubDir + L"\\config.json";

    std::ifstream file(path);
    if (!file.is_open()) {
        FACELOGIN_INFO(L"No config.json found, using defaults + registry");
        AppConfig cfg = DefaultConfig();
        // Fall back to registry match threshold if set
        float regThresh = 0.80f;
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FACELOGIN_REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD val = 0, size = sizeof(val);
            if (RegQueryValueExW(hKey, L"MatchThreshold", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS) {
                regThresh = val / 100.0f;
            }
            RegCloseKey(hKey);
        }
        cfg.match_threshold = regThresh;
        return cfg;
    }

    std::stringstream buf;
    buf << file.rdbuf();
    file.close();

    AppConfig cfg = ConfigFromJson(buf.str());
    FACELOGIN_INFO(L"Loaded config.json: thr=%.2f antiSpoof=%.3f camera=%hs rotation=%d",
                  cfg.match_threshold, cfg.anti_spoof_threshold, cfg.camera_device.c_str(),
                  cfg.camera_rotation);
    return cfg;
}

bool SaveConfig(const std::wstring& dataDir, const AppConfig& cfg) {
    std::wstring dir = ResolveDataDir(dataDir);
    std::wstring dataSubDir = dir + L"\\data";
    CreateDirectoryW(dataSubDir.c_str(), nullptr);
    std::wstring path = dataSubDir + L"\\config.json";

    std::string content = ConfigToJson(cfg);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        FACELOGIN_ERROR(L"Failed to write config.json: %s", path.c_str());
        return false;
    }
    file.write(content.c_str(), content.size());
    file.close();
    FACELOGIN_INFO(L"Saved config.json");
    return true;
}

} // namespace facelogin
