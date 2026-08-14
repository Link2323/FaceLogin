#include "sha256_util.h"
#include "model_failure.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

class TemporaryFile {
public:
    TemporaryFile() {
        m_path = std::filesystem::temp_directory_path() /
            (L"FaceLogin-ModelIntegrity-" +
             std::to_wstring(GetCurrentProcessId()) + L".bin");
    }

    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }

    const std::filesystem::path& Path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

// Lock-screen copy for a failed model bundle must be category-specific
// (detector/recognizer/PAD integrity vs a plain load error), actionable,
// and identical for the worker and standalone paths (single mapping source).
void TestModelFailureMessages() {
    using facelogin::ModelLoadFailure;
    const std::wstring detector =
        facelogin::ModelLoadFailureMessage(ModelLoadFailure::DetectorIntegrity);
    const std::wstring recognizer =
        facelogin::ModelLoadFailureMessage(ModelLoadFailure::RecognizerIntegrity);
    const std::wstring pad =
        facelogin::ModelLoadFailureMessage(ModelLoadFailure::PadIntegrity);
    const std::wstring load =
        facelogin::ModelLoadFailureMessage(ModelLoadFailure::Load);
    const std::wstring none =
        facelogin::ModelLoadFailureMessage(ModelLoadFailure::None);

    Check(detector.find(L"检测模型完整性校验失败") == 0 &&
              detector.find(L"重新安装 FaceLogin") != std::wstring::npos,
          "detector integrity copy names the category and the reinstall action");
    Check(recognizer.find(L"识别模型完整性校验失败") == 0 &&
              recognizer.find(L"重新安装 FaceLogin") != std::wstring::npos,
          "recognizer integrity copy names the category and the reinstall action");
    Check(pad.find(L"活体模型完整性校验失败") == 0 &&
              pad.find(L"重新安装 FaceLogin") != std::wstring::npos,
          "PAD integrity copy names the category and the reinstall action");
    Check(load.find(L"认证模型加载失败") == 0 && load == none,
          "generic load copy covers Load and unknown reasons");
    Check(detector != recognizer && detector != pad && detector != load &&
              recognizer != pad && recognizer != load && pad != load,
          "each failure category has distinct lock-screen copy");
}

} // namespace

int main() {
    TestModelFailureMessages();

    TemporaryFile file;
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output.write("abc", 3);
        Check(output.good(), "temporary model fixture is written");
    }

    constexpr const char* kAbcSha256 =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto actual = facelogin::FileSha256(file.Path());
    Check(actual && *actual == kAbcSha256,
          "FileSha256 returns the canonical digest");
    Check(facelogin::VerifyModelIntegrity(file.Path(), kAbcSha256,
                                           L"test model"),
          "matching model hash is accepted");

    std::string uppercase = kAbcSha256;
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::toupper(value));
                   });
    Check(facelogin::VerifyModelIntegrity(file.Path(), uppercase,
                                           L"test model"),
          "hash comparison is case-insensitive");

    std::string mismatch = kAbcSha256;
    mismatch.front() = mismatch.front() == '0' ? '1' : '0';
    Check(!facelogin::VerifyModelIntegrity(file.Path(), mismatch,
                                            L"test model"),
          "tampered model hash fails closed");

    std::error_code removeError;
    std::filesystem::remove(file.Path(), removeError);
    Check(!removeError, "temporary fixture is removed");
    Check(!facelogin::VerifyModelIntegrity(file.Path(), kAbcSha256,
                                            L"missing test model"),
          "missing model fails closed");

    if (g_failures != 0) {
        std::cerr << "ModelIntegrityTest: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "ModelIntegrityTest: all checks passed\n";
    return 0;
}
