#include "auth_worker_protocol.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

std::vector<float> UnitEmbedding() {
    using namespace facelogin::auth_worker;
    const float value = 1.0f / std::sqrt(static_cast<float>(kEmbeddingDimension));
    return std::vector<float>(kEmbeddingDimension, value);
}

std::vector<uint8_t> MakeConfigPayload(int32_t rotation,
                                       float threshold, int32_t timeout,
                                       uint32_t lowLight) {
    facelogin::auth_worker::PayloadWriter writer;
    writer.WriteI32(rotation);
    writer.WriteFloat(threshold);
    writer.WriteI32(timeout);
    writer.WriteU32(lowLight);
    writer.WriteWString(L"camera-path");
    return writer.Data();
}

void TestConfigRoundTrip() {
    using namespace facelogin::auth_worker;
    WorkerConfig input;
    input.cameraRotation = 270;
    input.antiSpoofThreshold = 0.381f;
    input.authTimeoutSeconds = 23;
    input.lowLightEnhance = true;
    input.cameraDevice = L"\\\\?\\usb#test-camera";

    WorkerConfig output;
    const auto encoded = EncodeConfig(input);
    Check(DecodeConfig(encoded, output), "valid worker configuration decodes");
    Check(output.cameraRotation == input.cameraRotation &&
          std::fabs(output.antiSpoofThreshold - input.antiSpoofThreshold) < 0.0001f &&
          output.authTimeoutSeconds == input.authTimeoutSeconds &&
          output.lowLightEnhance == input.lowLightEnhance &&
          output.cameraDevice == input.cameraDevice,
          "worker configuration round trip preserves every field");

    Check(!DecodeConfig(MakeConfigPayload(45, 0.281f, 15, 0), output),
          "invalid camera rotation is rejected");
    Check(!DecodeConfig(MakeConfigPayload(0, std::numeric_limits<float>::quiet_NaN(), 15, 0), output),
          "non-finite PAD threshold is rejected");
    Check(!DecodeConfig(MakeConfigPayload(0, 0.10f, 15, 0), output),
          "below-range PAD threshold is rejected");
    Check(DecodeConfig(MakeConfigPayload(0, 0.15f, 15, 0), output),
          "lower-bound PAD threshold 0.15 is accepted");
    Check(!DecodeConfig(MakeConfigPayload(0, 0.281f, 15, 2), output),
          "invalid low-light flag is rejected");

    auto truncated = encoded;
    truncated.pop_back();
    Check(!DecodeConfig(truncated, output), "truncated configuration is rejected");
    auto trailing = encoded;
    trailing.push_back(0);
    Check(!DecodeConfig(trailing, output), "configuration trailing bytes are rejected");
}

void TestStringRoundTrip() {
    using namespace facelogin::auth_worker;
    const std::wstring expected = L"正在检测人脸 — worker";
    std::wstring decoded;
    const auto encoded = EncodeWString(expected);
    Check(DecodeWString(encoded, decoded) && decoded == expected,
          "UTF-16 payload round trip preserves text");

    auto truncated = encoded;
    truncated.pop_back();
    Check(!DecodeWString(truncated, decoded), "truncated UTF-16 payload is rejected");
    Check(!DecodeWString(EncodeWString(std::wstring(9, L'x')), decoded, 8),
          "oversized UTF-16 payload is rejected");
}

void TestMatchProbeValidation() {
    using namespace facelogin::auth_worker;
    auto embedding = UnitEmbedding();
    unsigned int index = 99;
    std::vector<float> decoded;
    float preNorm = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    Check(DecodeMatchProbe(EncodeMatchProbe(2, embedding, 21.5f, -30.0f, 5.0f),
                           index, decoded, preNorm, yaw, pitch),
          "valid normalized 512-D match probe decodes");
    Check(index == 2 && decoded == embedding && preNorm == 21.5f &&
              std::fabs(yaw - (-30.0f)) < 1e-4f &&
              std::fabs(pitch - 5.0f) < 1e-4f,
          "match probe round trip preserves index, embedding, pre-norm and pose");

    auto invalid = embedding;
    invalid[17] = std::numeric_limits<float>::quiet_NaN();
    Check(!DecodeMatchProbe(EncodeMatchProbe(1, invalid, 21.5f, 0.0f, 0.0f),
                            index, decoded, preNorm, yaw, pitch),
          "NaN embedding value is rejected");
    invalid = embedding;
    invalid[17] = std::numeric_limits<float>::infinity();
    Check(!DecodeMatchProbe(EncodeMatchProbe(1, invalid, 21.5f, 0.0f, 0.0f),
                            index, decoded, preNorm, yaw, pitch),
          "infinite embedding value is rejected");
    Check(!DecodeMatchProbe(EncodeMatchProbe(0,
          std::vector<float>(kEmbeddingDimension, 0.0f), 21.5f, 0.0f, 0.0f),
          index, decoded, preNorm, yaw, pitch),
          "zero-norm embedding is rejected");
    invalid = embedding;
    for (float& value : invalid) value *= 0.5f;
    Check(!DecodeMatchProbe(EncodeMatchProbe(0, invalid, 21.5f, 0.0f, 0.0f),
                            index, decoded, preNorm, yaw, pitch),
          "abnormally small embedding norm is rejected");
    invalid = embedding;
    for (float& value : invalid) value *= 2.0f;
    Check(!DecodeMatchProbe(EncodeMatchProbe(0, invalid, 21.5f, 0.0f, 0.0f),
                            index, decoded, preNorm, yaw, pitch),
          "abnormally large embedding norm is rejected");

    PayloadWriter shortWriter;
    shortWriter.WriteU32(0);
    shortWriter.WriteFloatVector(std::vector<float>(511, 0.0f));
    Check(!DecodeMatchProbe(shortWriter.Data(), index, decoded, preNorm, yaw, pitch),
          "511-D match probe is rejected");
    PayloadWriter longWriter;
    longWriter.WriteU32(0);
    longWriter.WriteFloatVector(std::vector<float>(513, 0.0f));
    Check(!DecodeMatchProbe(longWriter.Data(), index, decoded, preNorm, yaw, pitch),
          "513-D match probe is rejected");
    PayloadWriter noNormWriter;
    noNormWriter.WriteU32(0);
    noNormWriter.WriteFloatVector(embedding);
    Check(!DecodeMatchProbe(noNormWriter.Data(), index, decoded, preNorm, yaw, pitch),
          "probe without trailing pre-norm (v3 payload) is rejected");
    PayloadWriter v4Writer;
    v4Writer.WriteU32(0);
    v4Writer.WriteFloatVector(embedding);
    v4Writer.WriteFloat(21.5f);
    Check(!DecodeMatchProbe(v4Writer.Data(), index, decoded, preNorm, yaw, pitch),
          "probe without pose angles (v4 payload) is rejected");
    Check(!DecodeMatchProbe(EncodeMatchProbe(0, embedding,
          std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f),
          index, decoded, preNorm, yaw, pitch),
          "NaN pre-norm is rejected");
    Check(!DecodeMatchProbe(EncodeMatchProbe(0, embedding, 0.0f, 0.0f, 0.0f),
                            index, decoded, preNorm, yaw, pitch),
          "zero pre-norm is rejected");
    Check(!DecodeMatchProbe(EncodeMatchProbe(3, embedding, 21.5f, 0.0f, 0.0f),
                            index, decoded, preNorm, yaw, pitch),
          "out-of-range binding index is rejected");
    Check(!DecodeMatchProbe(EncodeMatchProbe(0, embedding, 21.5f,
          std::numeric_limits<float>::quiet_NaN(), 0.0f),
          index, decoded, preNorm, yaw, pitch),
          "NaN yaw estimate is rejected");
    Check(!DecodeMatchProbe(EncodeMatchProbe(0, embedding, 21.5f, 0.0f,
          std::numeric_limits<float>::infinity()),
          index, decoded, preNorm, yaw, pitch),
          "infinite pitch estimate is rejected");
    Check(DecodeMatchProbe(EncodeMatchProbe(0, embedding, 21.5f, -90.0f, 90.0f),
                           index, decoded, preNorm, yaw, pitch),
          "physical range boundary yaw/pitch +/-90 is accepted");
    Check(!DecodeMatchProbe(EncodeMatchProbe(0, embedding, 21.5f, 90.5f, 0.0f),
                            index, decoded, preNorm, yaw, pitch),
          "out-of-physical-range yaw is rejected");
    auto trailing = EncodeMatchProbe(0, embedding, 21.5f, 0.0f, 0.0f);
    trailing.push_back(0);
    Check(!DecodeMatchProbe(trailing, index, decoded, preNorm, yaw, pitch),
          "match probe trailing bytes are rejected");
}

void TestAuthTimingValidation() {
    using namespace facelogin::auth_worker;
    const AuthTiming expected{405.5f, 718.25f, 1123.75f};
    AuthTiming decoded;
    const auto encoded = EncodeAuthTiming(expected);
    Check(DecodeAuthTiming(encoded, decoded),
          "valid authentication timing decodes");
    Check(decoded.cameraInitMs == expected.cameraInitMs &&
          decoded.pipelineMs == expected.pipelineMs &&
          decoded.totalMs == expected.totalMs,
          "authentication timing round trip preserves all fields");

    auto truncated = encoded;
    truncated.pop_back();
    Check(!DecodeAuthTiming(truncated, decoded),
          "truncated authentication timing is rejected");
    auto trailing = encoded;
    trailing.push_back(0);
    Check(!DecodeAuthTiming(trailing, decoded),
          "authentication timing trailing bytes are rejected");
    Check(!DecodeAuthTiming(EncodeAuthTiming(AuthTiming{
              std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f}), decoded),
          "NaN authentication timing is rejected");
    Check(!DecodeAuthTiming(EncodeAuthTiming(AuthTiming{
              1.0f, std::numeric_limits<float>::infinity(), 1.0f}), decoded),
          "infinite authentication timing is rejected");
    Check(!DecodeAuthTiming(EncodeAuthTiming(AuthTiming{-1.0f, 2.0f, 1.0f}), decoded),
          "negative authentication timing is rejected");
    Check(!DecodeAuthTiming(EncodeAuthTiming(AuthTiming{
              kMaxAuthTimingMs + 1.0f, 0.0f, kMaxAuthTimingMs + 1.0f}), decoded),
          "oversized authentication timing is rejected");
    Check(!DecodeAuthTiming(EncodeAuthTiming(AuthTiming{100.0f, 200.0f, 500.0f}), decoded),
          "inconsistent authentication timing total is rejected");
}

void TestAuthExchangeSequence() {
    using namespace facelogin::auth_worker;
    AuthExchangeValidator state(42);
    Check(state.HasExpectedRequestId(42), "matching request id is accepted");
    Check(!state.HasExpectedRequestId(41), "mismatched request id is rejected");
    Check(state.IsExpectedProbe(0), "first binding probe is expected");
    Check(state.IsExpectedProbe(0), "retry may repeat an unaccepted probe");
    Check(!state.CanSucceed(), "missing bindings cannot complete auth");
    Check(state.AcceptProbe(0), "first binding acceptance advances sequence");
    Check(!state.AcceptProbe(0), "accepted binding cannot be duplicated");
    Check(!state.IsExpectedProbe(2), "skipped binding index is rejected");
    Check(state.AcceptProbe(1), "second binding follows first");
    Check(!state.CanSucceed(), "two bindings cannot complete auth");
    Check(state.AcceptProbe(2), "third binding follows second");
    Check(state.CanSucceed(), "exactly three bindings permit success");
    Check(!state.AcceptProbe(2), "terminal binding cannot be duplicated");
}

struct RawChannel {
    HANDLE writer = nullptr;
    HANDLE unusedReader = nullptr;
    facelogin::auth_worker::Channel reader;
};

RawChannel MakeRawChannel() {
    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    HANDLE unusedRead = nullptr;
    HANDLE unusedWrite = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, nullptr, 0) ||
        !CreatePipe(&unusedRead, &unusedWrite, nullptr, 0)) {
        if (readHandle) CloseHandle(readHandle);
        if (writeHandle) CloseHandle(writeHandle);
        if (unusedRead) CloseHandle(unusedRead);
        if (unusedWrite) CloseHandle(unusedWrite);
        return {};
    }
    RawChannel result;
    result.writer = writeHandle;
    result.unusedReader = unusedRead;
    result.reader = facelogin::auth_worker::Channel(readHandle, unusedWrite);
    return result;
}

void CloseRawChannel(RawChannel& channel) {
    if (channel.writer) CloseHandle(channel.writer);
    if (channel.unusedReader) CloseHandle(channel.unusedReader);
    channel.writer = nullptr;
    channel.unusedReader = nullptr;
}

void CheckRawHeader(facelogin::auth_worker::MessageHeader header,
                    facelogin::auth_worker::ReadStatus expected,
                    const char* description) {
    using namespace facelogin::auth_worker;
    RawChannel channel = MakeRawChannel();
    Check(channel.writer != nullptr, "raw protocol pipe creation succeeds");
    if (!channel.writer) return;
    DWORD written = 0;
    const bool writeOk = WriteFile(channel.writer, &header, sizeof(header),
                                   &written, nullptr) != FALSE &&
                         written == sizeof(header);
    Message message;
    Check(writeOk && channel.reader.Read(message, 100) == expected, description);
    CloseRawChannel(channel);
}

void TestChannelFraming() {
    using namespace facelogin::auth_worker;

    HANDLE leftRead = nullptr;
    HANDLE rightWrite = nullptr;
    HANDLE rightRead = nullptr;
    HANDLE leftWrite = nullptr;
    Check(CreatePipe(&leftRead, &rightWrite, nullptr, 0) != FALSE,
          "create first anonymous pipe");
    Check(CreatePipe(&rightRead, &leftWrite, nullptr, 0) != FALSE,
          "create second anonymous pipe");
    if (!leftRead || !rightWrite || !rightRead || !leftWrite) return;

    {
        Channel left(leftRead, leftWrite);
        Channel right(rightRead, rightWrite);
        for (uint16_t raw = static_cast<uint16_t>(MessageType::Hello);
             raw <= static_cast<uint16_t>(MessageType::Cancel); ++raw) {
            const auto type = static_cast<MessageType>(raw);
            const auto payload = EncodeWString(L"round-trip");
            Check(left.Write(type, 0x1122334455667788ULL, payload),
                  "channel writes every known message type");
            Message received;
            Check(right.Read(received, 1000) == ReadStatus::Ok &&
                  received.type == type &&
                  received.requestId == 0x1122334455667788ULL &&
                  received.payload == payload,
                  "channel round trip preserves type, request id and payload");
        }

        const std::vector<uint8_t> maximumPayload(kMaxPayloadBytes, 0xA5);
        bool maximumWriteOk = false;
        std::thread maximumWriter([&]() {
            maximumWriteOk = left.Write(MessageType::Status, 99, maximumPayload);
        });
        Message maximumReceived;
        const ReadStatus maximumRead = right.Read(maximumReceived, 5000);
        maximumWriter.join();
        Check(maximumWriteOk && maximumRead == ReadStatus::Ok &&
              maximumReceived.type == MessageType::Status &&
              maximumReceived.requestId == 99 &&
              maximumReceived.payload == maximumPayload,
              "maximum 16 KiB payload is transferred without pipe-buffer deadlock");

        Check(!left.Write(MessageType::Status, 1,
                          std::vector<uint8_t>(kMaxPayloadBytes + 1, 0)),
              "channel refuses oversized outgoing payload");
    }

    MessageHeader badMagic;
    badMagic.type = static_cast<uint16_t>(MessageType::Status);
    badMagic.magic = 0;
    CheckRawHeader(badMagic, ReadStatus::Invalid, "bad protocol magic is rejected");
    MessageHeader badVersion;
    badVersion.type = static_cast<uint16_t>(MessageType::Status);
    badVersion.version = kProtocolVersion + 1;
    CheckRawHeader(badVersion, ReadStatus::Invalid, "bad protocol version is rejected");
    MessageHeader zeroType;
    CheckRawHeader(zeroType, ReadStatus::Invalid, "zero message type is rejected");
    MessageHeader unknownType;
    unknownType.type = static_cast<uint16_t>(MessageType::Cancel) + 1;
    CheckRawHeader(unknownType, ReadStatus::Invalid, "unknown message type is rejected");
    MessageHeader oversized;
    oversized.type = static_cast<uint16_t>(MessageType::Status);
    oversized.payloadSize = kMaxPayloadBytes + 1;
    CheckRawHeader(oversized, ReadStatus::Invalid, "oversized incoming payload is rejected");

    RawChannel partialHeader = MakeRawChannel();
    if (partialHeader.writer) {
        MessageHeader header;
        header.type = static_cast<uint16_t>(MessageType::Status);
        DWORD written = 0;
        WriteFile(partialHeader.writer, &header, sizeof(header) - 1, &written, nullptr);
        CloseHandle(partialHeader.writer);
        partialHeader.writer = nullptr;
        Message message;
        Check(partialHeader.reader.Read(message, 30) != ReadStatus::Ok,
              "truncated header is rejected");
        CloseRawChannel(partialHeader);
    }

    RawChannel partialPayload = MakeRawChannel();
    if (partialPayload.writer) {
        MessageHeader header;
        header.type = static_cast<uint16_t>(MessageType::Status);
        header.payloadSize = 4;
        DWORD written = 0;
        WriteFile(partialPayload.writer, &header, sizeof(header), &written, nullptr);
        const uint16_t halfPayload = 7;
        WriteFile(partialPayload.writer, &halfPayload, sizeof(halfPayload), &written, nullptr);
        CloseHandle(partialPayload.writer);
        partialPayload.writer = nullptr;
        Message message;
        Check(partialPayload.reader.Read(message, 30) != ReadStatus::Ok,
              "truncated payload is rejected");
        CloseRawChannel(partialPayload);
    }
}

} // namespace

int wmain() {
    TestConfigRoundTrip();
    TestStringRoundTrip();
    TestMatchProbeValidation();
    TestAuthTimingValidation();
    TestAuthExchangeSequence();
    TestChannelFraming();
    if (g_failures != 0) {
        std::fprintf(stderr, "AuthWorkerProtocolTest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("AuthWorkerProtocolTest: all checks passed");
    return 0;
}
