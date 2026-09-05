#include "auth_worker_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace facelogin::auth_worker {
namespace {

bool IsClosedError(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
           error == ERROR_NO_DATA || error == ERROR_INVALID_HANDLE;
}

DWORD RemainingTimeout(ULONGLONG start, DWORD requested) {
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (elapsed >= requested) return 0;
    return static_cast<DWORD>(requested - elapsed);
}

template <typename T>
void Append(std::vector<uint8_t>& out, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const size_t offset = out.size();
    out.resize(offset + sizeof(T));
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

} // namespace

void PayloadWriter::WriteU32(uint32_t value) { Append(m_data, value); }
void PayloadWriter::WriteI32(int32_t value) { Append(m_data, value); }
void PayloadWriter::WriteFloat(float value) { Append(m_data, value); }

void PayloadWriter::WriteWString(const std::wstring& value) {
    const uint32_t length = value.size() > std::numeric_limits<uint32_t>::max()
        ? 0 : static_cast<uint32_t>(value.size());
    WriteU32(length);
    if (length == 0) return;
    const size_t offset = m_data.size();
    m_data.resize(offset + static_cast<size_t>(length) * sizeof(wchar_t));
    std::memcpy(m_data.data() + offset, value.data(),
                static_cast<size_t>(length) * sizeof(wchar_t));
}

void PayloadWriter::WriteFloatVector(const std::vector<float>& value) {
    const uint32_t length = value.size() > std::numeric_limits<uint32_t>::max()
        ? 0 : static_cast<uint32_t>(value.size());
    WriteU32(length);
    if (length == 0) return;
    const size_t offset = m_data.size();
    m_data.resize(offset + static_cast<size_t>(length) * sizeof(float));
    std::memcpy(m_data.data() + offset, value.data(),
                static_cast<size_t>(length) * sizeof(float));
}

bool PayloadReader::ReadBytes(void* destination, size_t size) {
    if (size > m_data.size() - m_offset) return false;
    std::memcpy(destination, m_data.data() + m_offset, size);
    m_offset += size;
    return true;
}

bool PayloadReader::ReadU32(uint32_t& value) { return ReadBytes(&value, sizeof(value)); }
bool PayloadReader::ReadI32(int32_t& value) { return ReadBytes(&value, sizeof(value)); }
bool PayloadReader::ReadFloat(float& value) { return ReadBytes(&value, sizeof(value)); }

bool PayloadReader::ReadWString(std::wstring& value, uint32_t maxChars) {
    uint32_t length = 0;
    if (!ReadU32(length) || length > maxChars) return false;
    const size_t bytes = static_cast<size_t>(length) * sizeof(wchar_t);
    if (bytes > m_data.size() - m_offset) return false;
    value.assign(reinterpret_cast<const wchar_t*>(m_data.data() + m_offset), length);
    m_offset += bytes;
    return true;
}

bool PayloadReader::ReadFloatVector(std::vector<float>& value, uint32_t maxValues) {
    uint32_t length = 0;
    if (!ReadU32(length) || length > maxValues) return false;
    const size_t bytes = static_cast<size_t>(length) * sizeof(float);
    if (bytes > m_data.size() - m_offset) return false;
    value.resize(length);
    if (bytes != 0) std::memcpy(value.data(), m_data.data() + m_offset, bytes);
    m_offset += bytes;
    return true;
}

Channel::Channel(HANDLE readHandle, HANDLE writeHandle)
    : m_read(readHandle), m_write(writeHandle) {}

Channel::~Channel() { Close(); }

Channel::Channel(Channel&& other) noexcept
    : m_read(other.m_read), m_write(other.m_write) {
    other.m_read = INVALID_HANDLE_VALUE;
    other.m_write = INVALID_HANDLE_VALUE;
}

Channel& Channel::operator=(Channel&& other) noexcept {
    if (this == &other) return *this;
    Close();
    m_read = other.m_read;
    m_write = other.m_write;
    other.m_read = INVALID_HANDLE_VALUE;
    other.m_write = INVALID_HANDLE_VALUE;
    return *this;
}

bool Channel::IsValid() const {
    return m_read != INVALID_HANDLE_VALUE && m_read != nullptr &&
           m_write != INVALID_HANDLE_VALUE && m_write != nullptr;
}

bool Channel::WriteExact(const void* source, DWORD bytes) const {
    const auto* cursor = static_cast<const uint8_t*>(source);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(m_write, cursor, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool Channel::Write(MessageType type, uint64_t requestId,
                    const std::vector<uint8_t>& payload) {
    if (!IsValid() || payload.size() > kMaxPayloadBytes) return false;
    MessageHeader header;
    header.type = static_cast<uint16_t>(type);
    header.requestId = requestId;
    header.payloadSize = static_cast<uint32_t>(payload.size());
    if (!WriteExact(&header, sizeof(header))) return false;
    return payload.empty() || WriteExact(payload.data(), header.payloadSize);
}

ReadStatus Channel::WaitForBytes(DWORD bytes, DWORD timeoutMs,
                                 DWORD pollIntervalMs) const {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(m_read, nullptr, 0, nullptr, &available, nullptr)) {
            return IsClosedError(GetLastError()) ? ReadStatus::Closed : ReadStatus::IoError;
        }
        if (available >= bytes) return ReadStatus::Ok;
        if (RemainingTimeout(start, timeoutMs) == 0) return ReadStatus::Timeout;
        Sleep((std::max)(1UL, pollIntervalMs));
    }
}

ReadStatus Channel::ReadExact(void* destination, DWORD bytes, DWORD timeoutMs,
                              DWORD pollIntervalMs) const {
    auto* cursor = static_cast<uint8_t*>(destination);
    DWORD remaining = bytes;
    const ULONGLONG start = GetTickCount64();
    while (remaining > 0) {
        // Anonymous pipes may have a buffer smaller than the protocol's 16 KiB
        // payload ceiling. Waiting for the whole remaining frame before
        // reading can deadlock a synchronous writer once the pipe buffer is
        // full. Consume whatever is available and retain one deadline for the
        // complete frame.
        const ReadStatus waiting = WaitForBytes(
            1, RemainingTimeout(start, timeoutMs), pollIntervalMs);
        if (waiting != ReadStatus::Ok) return waiting;

        DWORD available = 0;
        if (!PeekNamedPipe(m_read, nullptr, 0, nullptr, &available, nullptr)) {
            return IsClosedError(GetLastError()) ? ReadStatus::Closed
                                                 : ReadStatus::IoError;
        }
        if (available == 0) continue;
        const DWORD chunk = (std::min)(remaining, available);
        DWORD read = 0;
        if (!ReadFile(m_read, cursor, chunk, &read, nullptr) || read == 0) {
            return IsClosedError(GetLastError()) ? ReadStatus::Closed : ReadStatus::IoError;
        }
        cursor += read;
        remaining -= read;
    }
    return ReadStatus::Ok;
}

ReadStatus Channel::Read(Message& message, DWORD timeoutMs,
                         DWORD pollIntervalMs) {
    message = {};
    if (!IsValid()) return ReadStatus::Closed;

    const ULONGLONG start = GetTickCount64();
    MessageHeader header;
    ReadStatus status = ReadExact(&header, sizeof(header), timeoutMs,
                                  pollIntervalMs);
    if (status != ReadStatus::Ok) return status;
    if (header.magic != kProtocolMagic || header.version != kProtocolVersion ||
        header.payloadSize > kMaxPayloadBytes || header.type == 0 ||
        header.type > static_cast<uint16_t>(MessageType::Cancel)) {
        return ReadStatus::Invalid;
    }

    message.type = static_cast<MessageType>(header.type);
    message.requestId = header.requestId;
    message.payload.resize(header.payloadSize);
    if (header.payloadSize == 0) return ReadStatus::Ok;
    return ReadExact(message.payload.data(), header.payloadSize,
                     RemainingTimeout(start, timeoutMs), pollIntervalMs);
}

void Channel::Close() {
    if (m_read != INVALID_HANDLE_VALUE && m_read != nullptr) {
        CloseHandle(m_read);
        m_read = INVALID_HANDLE_VALUE;
    }
    if (m_write != INVALID_HANDLE_VALUE && m_write != nullptr) {
        CloseHandle(m_write);
        m_write = INVALID_HANDLE_VALUE;
    }
}

std::vector<uint8_t> EncodeConfig(const WorkerConfig& config) {
    PayloadWriter writer;
    writer.WriteI32(config.cameraRotation);
    writer.WriteFloat(config.antiSpoofThreshold);
    writer.WriteI32(config.authTimeoutSeconds);
    writer.WriteU32(config.lowLightEnhance ? 1U : 0U);
    writer.WriteWString(config.cameraDevice);
    return writer.Data();
}

bool DecodeConfig(const std::vector<uint8_t>& payload, WorkerConfig& config) {
    PayloadReader reader(payload);
    int32_t rotation = 0;
    int32_t timeout = 0;
    uint32_t lowLight = 0;
    if (!reader.ReadI32(rotation) ||
        !reader.ReadFloat(config.antiSpoofThreshold) || !reader.ReadI32(timeout) ||
        !reader.ReadU32(lowLight) ||
        !reader.ReadWString(config.cameraDevice) ||
        !reader.Done()) {
        return false;
    }
    if ((rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) ||
        !std::isfinite(config.antiSpoofThreshold) ||
        config.antiSpoofThreshold < 0.15f || config.antiSpoofThreshold > 0.50f ||
        timeout < 1 || timeout > 60 || lowLight > 1) {
        return false;
    }
    config.cameraRotation = rotation;
    config.authTimeoutSeconds = timeout;
    config.lowLightEnhance = lowLight != 0;
    return true;
}

std::vector<uint8_t> EncodeWString(const std::wstring& value) {
    PayloadWriter writer;
    writer.WriteWString(value);
    return writer.Data();
}

bool DecodeWString(const std::vector<uint8_t>& payload, std::wstring& value,
                   uint32_t maxChars) {
    PayloadReader reader(payload);
    return reader.ReadWString(value, maxChars) && reader.Done();
}

std::vector<uint8_t> EncodeMatchProbe(unsigned int bindingIndex,
                                      const std::vector<float>& embedding,
                                      float preNorm,
                                      float yawDeg,
                                      float pitchDeg) {
    PayloadWriter writer;
    writer.WriteU32(bindingIndex);
    writer.WriteFloatVector(embedding);
    writer.WriteFloat(preNorm);
    writer.WriteFloat(yawDeg);
    writer.WriteFloat(pitchDeg);
    return writer.Data();
}

bool DecodeMatchProbe(const std::vector<uint8_t>& payload,
                      unsigned int& bindingIndex,
                      std::vector<float>& embedding,
                      float& preNorm,
                      float& yawDeg,
                      float& pitchDeg) {
    PayloadReader reader(payload);
    uint32_t rawIndex = 0;
    if (!reader.ReadU32(rawIndex) || rawIndex > 2 ||
        !reader.ReadFloatVector(embedding, kEmbeddingDimension) ||
        !reader.ReadFloat(preNorm) || !reader.ReadFloat(yawDeg) ||
        !reader.ReadFloat(pitchDeg) || !reader.Done() ||
        embedding.size() != kEmbeddingDimension) {
        return false;
    }
    double normSquared = 0.0;
    for (float value : embedding) {
        if (!std::isfinite(value)) return false;
        normSquared += static_cast<double>(value) * value;
    }
    const double minNormSquared =
        static_cast<double>(kMinEmbeddingNorm) * kMinEmbeddingNorm;
    const double maxNormSquared =
        static_cast<double>(kMaxEmbeddingNorm) * kMaxEmbeddingNorm;
    if (!std::isfinite(normSquared) || normSquared < minNormSquared ||
        normSquared > maxNormSquared) return false;
    // The quality signal travels alongside the (unit-length) embedding: a
    // finite, strictly positive value lets the parent gate template updates
    // on recognizer norm without recomputing it from the normalized vector.
    if (!std::isfinite(preNorm) || preNorm <= 0.0f) return false;
    // Pose estimates come from atan-based weak-perspective models, so the
    // physical range is ±90°; anything outside is wire corruption.
    if (!std::isfinite(yawDeg) || std::fabs(yawDeg) > 90.0f ||
        !std::isfinite(pitchDeg) || std::fabs(pitchDeg) > 90.0f) {
        return false;
    }
    bindingIndex = rawIndex;
    return true;
}

std::vector<uint8_t> EncodeAuthTiming(const AuthTiming& timing) {
    PayloadWriter writer;
    writer.WriteFloat(timing.cameraInitMs);
    writer.WriteFloat(timing.pipelineMs);
    writer.WriteFloat(timing.totalMs);
    return writer.Data();
}

bool DecodeAuthTiming(const std::vector<uint8_t>& payload, AuthTiming& timing) {
    PayloadReader reader(payload);
    if (!reader.ReadFloat(timing.cameraInitMs) ||
        !reader.ReadFloat(timing.pipelineMs) ||
        !reader.ReadFloat(timing.totalMs) || !reader.Done()) {
        return false;
    }
    if (!std::isfinite(timing.cameraInitMs) ||
        !std::isfinite(timing.pipelineMs) ||
        !std::isfinite(timing.totalMs) ||
        timing.cameraInitMs < 0.0f || timing.pipelineMs < 0.0f ||
        timing.totalMs < 0.0f ||
        timing.cameraInitMs > kMaxAuthTimingMs ||
        timing.pipelineMs > kMaxAuthTimingMs ||
        timing.totalMs > kMaxAuthTimingMs) {
        return false;
    }
    return std::fabs((timing.cameraInitMs + timing.pipelineMs) - timing.totalMs) <=
        kAuthTimingConsistencyToleranceMs;
}

} // namespace facelogin::auth_worker
