#include "../../common/ipc_protocol.h"

#include <cstdio>
#include <string>

namespace {

using facelogin::ipc::AuthResult;

int g_failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++g_failures;
    }
}

void TestMsaRoundTrip() {
    const std::wstring message = facelogin::ipc::BuildAuthSuccessMessage(
        L"S-1-5-21-100", L"alice@example.test", L"DESKTOP", L"alice",
        L"p:a:ss:word");
    const AuthResult parsed = facelogin::ipc::ParseAuthMessage(message);
    Check(parsed.status == AuthResult::Status::Success,
          "MSA success message parses");
    Check(parsed.sid == L"S-1-5-21-100" &&
          parsed.upn == L"alice@example.test" &&
          parsed.domain == L"DESKTOP" && parsed.username == L"alice" &&
          parsed.password == L"p:a:ss:word",
          "MSA fields and password colons survive round trip");
}

void TestLocalAccountRoundTrip() {
    const std::wstring message = facelogin::ipc::BuildAuthSuccessMessage(
        L"S-1-5-21-200", L"", L"DESKTOP", L"local-user", L"local-password");
    Check(message ==
              L"AUTH_SUCCESS:S-1-5-21-200::DESKTOP\\local-user:local-password",
          "local account wire format preserves the empty UPN field");
    const AuthResult parsed = facelogin::ipc::ParseAuthMessage(message);
    Check(parsed.status == AuthResult::Status::Success &&
          parsed.sid == L"S-1-5-21-200" && parsed.upn.empty() &&
          parsed.domain == L"DESKTOP" && parsed.username == L"local-user" &&
          parsed.password == L"local-password",
          "local account success message round trips");
}

void TestLegacyFormatIsRejected() {
    const AuthResult domainUser = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:OLDPC\\legacy:old:password");
    Check(domainUser.status == AuthResult::Status::Error,
          "legacy DOMAIN\\USER success format is rejected");

    const AuthResult bareUser = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:legacy:password");
    Check(bareUser.status == AuthResult::Status::Error,
          "legacy bare username success format is rejected");

    const AuthResult colonHeavyLegacy = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:OLDPC\\legacy:one:two:three");
    Check(colonHeavyLegacy.status == AuthResult::Status::Error,
          "legacy success message with multiple password colons is rejected");

    const AuthResult missingDomain = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:S-1-5-21-100::local-user:password");
    Check(missingDomain.status == AuthResult::Status::Error,
          "current-format success message without DOMAIN\\USER is rejected");
}

void TestTerminalMessages() {
    Check(facelogin::ipc::ParseAuthMessage(L"AUTH_TIMEOUT").status ==
              AuthResult::Status::Timeout,
          "timeout terminal parses");
    const std::wstring errorMessage =
        facelogin::ipc::BuildAuthErrorMessage(L"camera unavailable");
    const AuthResult error = facelogin::ipc::ParseAuthMessage(errorMessage);
    Check(error.status == AuthResult::Status::Error &&
          error.errorMessage == L"camera unavailable",
          "error terminal round trips");
}

void TestMalformedMessagesFailClosed() {
    const AuthResult empty = facelogin::ipc::ParseAuthMessage(L"");
    Check(empty.status == AuthResult::Status::Error && !empty.errorMessage.empty(),
          "empty message fails closed");

    const AuthResult truncated = facelogin::ipc::ParseAuthMessage(
        L"AUTH_SUCCESS:no-password-separator");
    Check(truncated.status == AuthResult::Status::Error &&
          !truncated.errorMessage.empty(),
          "truncated success message fails closed");

    const AuthResult unknown = facelogin::ipc::ParseAuthMessage(L"UNRECOGNIZED");
    Check(unknown.status == AuthResult::Status::Error &&
          !unknown.errorMessage.empty(),
          "unknown message fails closed");
}

} // namespace

int wmain() {
    TestMsaRoundTrip();
    TestLocalAccountRoundTrip();
    TestLegacyFormatIsRejected();
    TestTerminalMessages();
    TestMalformedMessagesFailClosed();
    if (g_failures != 0) {
        std::fprintf(stderr, "IpcProtocolTest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("IpcProtocolTest: all checks passed");
    return 0;
}
