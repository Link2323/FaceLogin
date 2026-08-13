#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace facelogin::auth_worker {

inline constexpr uint32_t kProtocolMagic = 0x4B574C46; // "FLWK"
inline constexpr uint16_t kProtocolVersion = 3;
inline constexpr uint32_t kMaxPayloadBytes = 16 * 1024;
inline constexpr uint32_t kEmbeddingDimension = 512;
// The recognizer L2-normalizes every production embedding before it crosses
// the worker boundary. Keep a little numerical tolerance, but reject zero,
// denormalized and attacker-scaled vectors before the parent matcher sees them.
inline constexpr float kMinEmbeddingNorm = 0.90f;
inline constexpr float kMaxEmbeddingNorm = 1.10f;
inline constexpr float kMaxAuthTimingMs = 120000.0f;
inline constexpr float kAuthTimingConsistencyToleranceMs = 10.0f;

enum class MessageType : uint16_t {
    Hello = 1,
    Init = 2,
    Ready = 3,
    StartAuth = 4,
    Status = 5,
    MatchProbe = 6,
    MatchAccept = 7,
    MatchRetry = 8,
    MatchReject = 9,
    AuthSucceeded = 10,
    AuthFailed = 11,
    AuthTimedOut = 12,
    Fatal = 13,
    Cancel = 14,
};

enum class ReadStatus {
    Ok,
    Timeout,
    Closed,
    Invalid,
    IoError,
};

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic = kProtocolMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = 0;
    uint64_t requestId = 0;
    uint32_t payloadSize = 0;
};
#pragma pack(pop)
static_assert(sizeof(MessageHeader) == 20, "worker protocol header layout changed");

struct Message {
    MessageType type = MessageType::Fatal;
    uint64_t requestId = 0;
    std::vector<uint8_t> payload;
};

// Pure parent-side state machine for one authentication exchange. Keeping the
// request-id and binding-order checks here makes the fail-closed rules directly
// unit-testable instead of duplicating ad-hoc counters in the supervisor.
class AuthExchangeValidator {
public:
    explicit AuthExchangeValidator(uint64_t requestId) : m_requestId(requestId) {}

    bool HasExpectedRequestId(uint64_t requestId) const {
        return requestId == m_requestId;
    }
    bool IsExpectedProbe(unsigned int bindingIndex) const {
        return m_acceptedBindings < 3 && bindingIndex == m_acceptedBindings;
    }
    bool AcceptProbe(unsigned int bindingIndex) {
        if (!IsExpectedProbe(bindingIndex)) return false;
        ++m_acceptedBindings;
        return true;
    }
    bool CanSucceed() const { return m_acceptedBindings == 3; }
    unsigned int AcceptedBindings() const { return m_acceptedBindings; }

private:
    uint64_t m_requestId = 0;
    unsigned int m_acceptedBindings = 0;
};

class PayloadWriter {
public:
    void WriteU32(uint32_t value);
    void WriteI32(int32_t value);
    void WriteFloat(float value);
    void WriteWString(const std::wstring& value);
    void WriteFloatVector(const std::vector<float>& value);
    const std::vector<uint8_t>& Data() const { return m_data; }

private:
    std::vector<uint8_t> m_data;
};

class PayloadReader {
public:
    explicit PayloadReader(const std::vector<uint8_t>& data) : m_data(data) {}

    bool ReadU32(uint32_t& value);
    bool ReadI32(int32_t& value);
    bool ReadFloat(float& value);
    bool ReadWString(std::wstring& value, uint32_t maxChars = 2048);
    bool ReadFloatVector(std::vector<float>& value, uint32_t maxValues);
    bool Done() const { return m_offset == m_data.size(); }

private:
    bool ReadBytes(void* destination, size_t size);

    const std::vector<uint8_t>& m_data;
    size_t m_offset = 0;
};

// Owns one read and one write end of the private parent/worker channels.
class Channel {
public:
    Channel() = default;
    Channel(HANDLE readHandle, HANDLE writeHandle);
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;

    bool IsValid() const;
    bool Write(MessageType type, uint64_t requestId,
               const std::vector<uint8_t>& payload = {});
    ReadStatus Read(Message& message, DWORD timeoutMs,
                    DWORD pollIntervalMs = 5);
    void Close();

private:
    ReadStatus WaitForBytes(DWORD bytes, DWORD timeoutMs,
                            DWORD pollIntervalMs) const;
    ReadStatus ReadExact(void* destination, DWORD bytes, DWORD timeoutMs,
                         DWORD pollIntervalMs) const;
    bool WriteExact(const void* source, DWORD bytes) const;

    HANDLE m_read = INVALID_HANDLE_VALUE;
    HANDLE m_write = INVALID_HANDLE_VALUE;
};

struct WorkerConfig {
    int cameraRotation = 0;
    float antiSpoofThreshold = 0.281f;
    int authTimeoutSeconds = 15;
    bool lowLightEnhance = false;
    std::wstring cameraDevice;
};

// Success-path timing is returned with the terminal message so the worker can
// hand control back before doing any synchronous file logging. These values
// are diagnostic only and never participate in the authentication decision.
struct AuthTiming {
    float cameraInitMs = 0.0f;
    float pipelineMs = 0.0f;
    float totalMs = 0.0f;
};

std::vector<uint8_t> EncodeConfig(const WorkerConfig& config);
bool DecodeConfig(const std::vector<uint8_t>& payload, WorkerConfig& config);

std::vector<uint8_t> EncodeWString(const std::wstring& value);
bool DecodeWString(const std::vector<uint8_t>& payload, std::wstring& value,
                   uint32_t maxChars = 2048);

std::vector<uint8_t> EncodeMatchProbe(unsigned int bindingIndex,
                                      const std::vector<float>& embedding);
bool DecodeMatchProbe(const std::vector<uint8_t>& payload,
                      unsigned int& bindingIndex,
                      std::vector<float>& embedding);

std::vector<uint8_t> EncodeAuthTiming(const AuthTiming& timing);
bool DecodeAuthTiming(const std::vector<uint8_t>& payload, AuthTiming& timing);

} // namespace facelogin::auth_worker
