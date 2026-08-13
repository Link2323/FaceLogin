#include "sha256_util.h"

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

} // namespace

int main() {
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
