#include "pipe_server.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include <sddl.h>
#include <vector>

namespace facelogin {

std::atomic<long> PipeServer::g_aclAllocations{0};

PipeServer::PipeSecurity::~PipeSecurity() {
    // Absolute-format SDs do not own their DACL: both allocations must be
    // freed separately, and keeping the two frees adjacent here means there
    // is exactly one ownership path (no caller can forget the ACL).
    if (acl) {
        LocalFree(acl);
        --g_aclAllocations;
    }
    if (sd) {
        LocalFree(sd);
    }
}

PipeServer::~PipeServer() {
    Close();
}

bool PipeServer::CreateSecurityDescriptor(PipeSecurity& out) {
    // Create a security descriptor that grants access to:
    // - SYSTEM (full control)
    // - BUILTIN\Administrators (full control)
    // - Current interactive user (full control)
    // Deny: Network, Anonymous, Everyone else

    EXPLICIT_ACCESSW ea[3] = {};

    // SYSTEM: full access
    ea[0].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName = const_cast<LPWSTR>(L"SYSTEM");

    // Administrators: full access
    ea[1].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    ea[1].grfAccessMode = SET_ACCESS;
    ea[1].grfInheritance = NO_INHERITANCE;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea[1].Trustee.ptstrName = const_cast<LPWSTR>(L"Administrators");

    DWORD entryCount = 2;

    // Try to add the current user by name.
    // GetUserNameW works even when running elevated.
    wchar_t currentUserName[256] = {};
    DWORD nameLen = 256;
    if (GetUserNameW(currentUserName, &nameLen) && nameLen > 0) {
        // Build domain\user format for SetEntriesInAclW
        wchar_t qualifiedName[512] = {};

        DWORD sidBufSize = 0;
        DWORD domainLen = 256;
        SID_NAME_USE sidType;

        // First call to get buffer size
        LookupAccountNameW(nullptr, currentUserName,
                          nullptr, &sidBufSize,
                          nullptr, &domainLen, &sidType);

        if (sidBufSize > 0) {
            std::vector<BYTE> sidBuf(sidBufSize);
            wchar_t domain[256] = {};
            DWORD domainSz = 256;
            if (LookupAccountNameW(nullptr, currentUserName,
                                   sidBuf.data(), &sidBufSize,
                                   domain, &domainSz, &sidType)) {
                swprintf_s(qualifiedName, L"%s\\%s", domain, currentUserName);
            } else {
                wcscpy_s(qualifiedName, currentUserName);
            }
        } else {
            wcscpy_s(qualifiedName, currentUserName);
        }

        ea[2].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
        ea[2].grfAccessMode = SET_ACCESS;
        ea[2].grfInheritance = NO_INHERITANCE;
        ea[2].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
        ea[2].Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea[2].Trustee.ptstrName = qualifiedName;
        entryCount = 3;
        // ACL composition detail — DEBUG; fires on every reconnect (once per
        // auth). The "Named pipe created" line is the INFO-level marker.
        FACELOGIN_DEBUG(L"Pipe ACL: added current user %s", qualifiedName);
    }

    DWORD dwErr = SetEntriesInAclW(entryCount, ea, nullptr, &out.acl);

    if (dwErr != ERROR_SUCCESS) {
        FACELOGIN_ERROR(L"SetEntriesInAcl failed: %lu", dwErr);
        return false;
    }
    ++g_aclAllocations;  // SetEntriesInAclW allocated the ACL (LocalFree-owned)

    PSECURITY_DESCRIPTOR pSD = static_cast<PSECURITY_DESCRIPTOR>(
        LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
    bool ok = pSD != nullptr &&
              InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION) != FALSE &&
              SetSecurityDescriptorDacl(pSD, TRUE, out.acl, FALSE) != FALSE;
    if (!ok) {
        if (pSD) LocalFree(pSD);
        LocalFree(out.acl);
        --g_aclAllocations;
        out.acl = nullptr;
        return false;
    }

    out.sd = pSD;
    return true;
}

bool PipeServer::CreatePipeInstance(DWORD timeoutMs, const wchar_t* pipeName) {
    Close(); // Ensure clean state

    const wchar_t* name = pipeName ? pipeName : ipc::PIPE_NAME;

    PipeSecurity sec;
    if (!CreateSecurityDescriptor(sec)) return false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = sec.sd;
    sa.bInheritHandle = FALSE;

    // Message reads/writes remain synchronous. PIPE_NOWAIT is used only while
    // accepting a connection; WaitForClient polls ConnectNamedPipe and can
    // therefore observe RequestShutdown without relying on CloseHandle from
    // another thread to cancel a synchronous operation.
    m_hPipe = CreateNamedPipeW(
        name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1,                                          // Max 1 instance
        ipc::PIPE_BUFFER_SIZE,
        ipc::PIPE_BUFFER_SIZE,
        timeoutMs,                                  // default timeout for pipe ops
        &sa);

    // CreateNamedPipeW has copied the descriptor into the new pipe instance;
    // our absolute-format SD and its DACL are no longer needed. `sec`'s
    // destructor frees both (LocalFree SD + ACL) on scope exit — the ACL is
    // NOT owned by the SD, so freeing only the SD (as before) leaked one ACL
    // per pipe instance (once per auth round).
    if (m_hPipe == INVALID_HANDLE_VALUE) {
        FACELOGIN_ERROR(L"CreateNamedPipe failed: %lu", GetLastError());
        return false;
    }

        // Per-reconnect marker — DEBUG; fires once per auth. The request
        // dispatch ("Received request: ...") logged in FaceService::Run is the
        // INFO-level signal that a client arrived.
        FACELOGIN_DEBUG(L"Named pipe created, waiting for client...");

    return true;
}

bool PipeServer::WaitForClient(DWORD timeoutMs, const wchar_t* pipeName) {
    if (m_shutdownRequested.load()) return false;
    if (!CreatePipeInstance(timeoutMs, pipeName)) return false;

    constexpr DWORD kPollIntervalMs = 25;
    DWORD waited = 0;
    while (!m_shutdownRequested.load() && waited < timeoutMs) {
        if (ConnectNamedPipe(m_hPipe, nullptr)) {
            m_connected = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                m_connected = true;
            } else if (err == ERROR_PIPE_LISTENING) {
                const DWORD sleepMs = (timeoutMs - waited < kPollIntervalMs)
                    ? (timeoutMs - waited) : kPollIntervalMs;
                Sleep(sleepMs);
                waited += sleepMs;
                continue;
            } else if (err == ERROR_NO_DATA || err == ERROR_PIPE_NOT_CONNECTED) {
                // A client connected and went away before the service accepted
                // it. Reset this instance and continue waiting within the same
                // bounded interval.
                DisconnectNamedPipe(m_hPipe);
                continue;
            } else {
                FACELOGIN_WARN(L"Client connection failed: %lu", err);
                Close();
                return false;
            }
        }

        DWORD mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
        if (!SetNamedPipeHandleState(m_hPipe, &mode, nullptr, nullptr)) {
            FACELOGIN_ERROR(L"SetNamedPipeHandleState(PIPE_WAIT) failed: %lu", GetLastError());
            Close();
            return false;
        }
        FACELOGIN_DEBUG(L"Client connected");
        return true;
    }

    if (m_shutdownRequested.load()) {
        FACELOGIN_INFO(L"Pipe wait cancelled by service stop request");
    }
    Close();
    return false;
}

bool PipeServer::ReadMessage(std::wstring& outMessage, DWORD timeoutMs) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return false;

    // Bounded synchronous read on a message-mode pipe. ReadFile on a sync
    // pipe would block forever if the client never sends/never disconnects,
    // so we first poll PeekNamedPipe for either available data or a broken
    // connection, up to timeoutMs.
    DWORD bytesAvail = 0, totalBytes = 0;
    for (DWORD waited = 0; waited < timeoutMs; ) {
        if (m_shutdownRequested.load()) return false;
        if (PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvail, &totalBytes)) {
            if (bytesAvail > 0) {
                // Data ready — the ReadFile below returns immediately.
                break;
            }
            // Connected but idle — wait briefly, keep polling.
            DWORD sleepMs = (timeoutMs - waited < 50) ? (timeoutMs - waited) : 50;
            Sleep(sleepMs);
            waited += sleepMs;
            continue;
        }
        // PeekNamedPipe failed — client closed its end or pipe is broken.
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
            err == ERROR_PIPE_NOT_CONNECTED) {
            FACELOGIN_INFO(L"Pipe broken by client while waiting for message");
            m_connected = false;
            return false;
        }
        FACELOGIN_ERROR(L"PeekNamedPipe failed: %lu", err);
        m_connected = false;
        return false;
    }

    // Polling window exhausted. Re-check once: a message that arrived just as
    // the window closed must still be read, but a connected-and-silent client
    // must NOT fall through to the synchronous ReadFile below — that would
    // block forever and let one client hold the single pipe instance hostage
    // (all later unlock attempts fail). Return a timeout instead; the caller
    // disconnects and accepts the next client.
    DWORD bytesAvailAfter = 0, totalBytesAfter = 0;
    if (!PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvailAfter, &totalBytesAfter)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
            err == ERROR_PIPE_NOT_CONNECTED) {
            FACELOGIN_INFO(L"Pipe broken by client at read deadline");
        } else {
            FACELOGIN_ERROR(L"PeekNamedPipe failed at read deadline: %lu", err);
        }
        m_connected = false;
        return false;
    }
    if (bytesAvailAfter == 0) {
        // Client is alive but silent past the deadline. Keep m_connected so
        // the caller can Disconnect() cleanly; never block on ReadFile.
        FACELOGIN_WARN(L"ReadMessage timed out after %lu ms with no data", timeoutMs);
        return false;
    }

    wchar_t buffer[ipc::PIPE_BUFFER_SIZE / sizeof(wchar_t)] = {};
    DWORD bytesRead = 0;
    BOOL result = ReadFile(m_hPipe, buffer,
                           static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                           &bytesRead, nullptr);

    if (!result || bytesRead == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            FACELOGIN_INFO(L"Pipe broken by client");
        } else if (err == ERROR_MORE_DATA) {
            // Message larger than the fixed buffer; the protocol has no
            // fragmentation. Fail closed and let the caller disconnect.
            FACELOGIN_ERROR(L"ReadMessage: message exceeds %u bytes; "
                            L"closing connection", ipc::PIPE_BUFFER_SIZE);
        } else if (!result) {
            FACELOGIN_ERROR(L"ReadFile failed: %lu", err);
        }
        m_connected = false;
        return false;
    }

    size_t len = bytesRead / sizeof(wchar_t);
    // Strip trailing null terminator(s) added by pipe WriteFile
    while (len > 0 && buffer[len - 1] == L'\0') {
        len--;
    }
    outMessage.assign(buffer, len);
    return true;
}

// Bounded "wait for the client to drain what we wrote". Previously the code
// did an unbounded ReadFile(dummy) after every WriteMessage to handshake the
// disconnect; if the client never read or never closed, the service blocked
// forever (SCM killed it → 7034, no crash event). We poll PeekNamedPipe:
// when no bytes remain to be read, the client has consumed everything (or
// closed its end) and it is safe to Disconnect().
bool PipeServer::DrainOutput(DWORD timeoutMs) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return true;

    DWORD bytesAvail = 0, totalBytes = 0;
    for (DWORD waited = 0; waited < timeoutMs; ) {
        if (m_shutdownRequested.load()) return false;
        if (PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvail, &totalBytes)) {
            if (bytesAvail == 0) {
                // All output consumed (or client already closed). Done.
                return true;
            }
            DWORD sleepMs = (timeoutMs - waited < 50) ? (timeoutMs - waited) : 50;
            Sleep(sleepMs);
            waited += sleepMs;
            continue;
        }
        // PeekNamedPipe failed → client closed its end. Fine, nothing to drain.
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
            err == ERROR_PIPE_NOT_CONNECTED) {
            m_connected = false;
            return true;
        }
        FACELOGIN_ERROR(L"DrainOutput: PeekNamedPipe failed: %lu", err);
        return false;
    }
    // Timed out with unread bytes still pending — do NOT block further.
    FACELOGIN_WARN(L"DrainOutput: timed out with unread bytes pending");
    return false;
}

bool PipeServer::WriteMessage(const std::wstring& message) {
    if (m_shutdownRequested.load() || !m_connected || m_hPipe == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    DWORD byteSize = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));

    BOOL result = WriteFile(m_hPipe, message.c_str(), byteSize,
                            &bytesWritten, nullptr);

    if (!result || bytesWritten == 0) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            FACELOGIN_INFO(L"Pipe broken during write");
        } else if (!result) {
            FACELOGIN_ERROR(L"WriteFile failed: %lu", err);
        }
        m_connected = false;
        return false;
    }

    return true;
}

bool PipeServer::IsClientDisconnected() const {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE) return true;

    // PeekNamedPipe with null buffers never blocks and reports the pipe state.
    // ERROR_BROKEN_PIPE / ERROR_NO_DATA => client closed its end.
    DWORD bytesAvail = 0, bytesLeft = 0;
    if (PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvail, &bytesLeft)) {
        return false; // still connected
    }
    DWORD err = GetLastError();
    return err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA ||
           err == ERROR_PIPE_NOT_CONNECTED;
}

void PipeServer::Disconnect() {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_hPipe);
        DisconnectNamedPipe(m_hPipe);
        m_connected = false;
    }
}

void PipeServer::RequestShutdown() {
    m_shutdownRequested.store(true);
}

void PipeServer::Close() {
    Disconnect();

    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }

    m_connected = false;
}

} // namespace facelogin
