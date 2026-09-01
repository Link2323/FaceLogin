#include "pipe_server.h"
#include "../common/logger.h"
#include "../common/ipc_protocol.h"
#include <sddl.h>
#include <vector>

namespace facelogin {

namespace {

constexpr DWORD kPipeWriteTimeoutMs = 5000;

bool IsPipeClosedError(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
           error == ERROR_PIPE_NOT_CONNECTED;
}

// A stack OVERLAPPED and its buffer may not go out of scope until the kernel
// has completed or cancelled the operation. This helper performs the required
// cancel-and-reap sequence and intentionally ignores ERROR_NOT_FOUND (the I/O
// won the race and GetOverlappedResult still observes its final state).
void CancelAndReap(HANDLE pipe, OVERLAPPED& overlapped) {
    CancelIoEx(pipe, &overlapped);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
}

} // namespace

std::atomic<long> PipeServer::g_aclAllocations{0};

PipeServer::PipeServer() {
    m_hShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

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
    if (m_hShutdownEvent) {
        CloseHandle(m_hShutdownEvent);
        m_hShutdownEvent = nullptr;
    }
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

    // Every operation on an OVERLAPPED handle supplies its own OVERLAPPED
    // state. PIPE_WAIT keeps ordinary message-mode semantics; cancellation is
    // driven by m_hShutdownEvent + CancelIoEx rather than PIPE_NOWAIT polling.
    m_hPipe = CreateNamedPipeW(
        name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
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
    if (m_shutdownRequested.load() || !m_hShutdownEvent) return false;
    if (!CreatePipeInstance(timeoutMs, pipeName)) return false;

    HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connectEvent) {
        FACELOGIN_ERROR(L"CreateEvent for pipe connect failed: %lu", GetLastError());
        Close();
        return false;
    }
    OVERLAPPED overlapped = {};
    overlapped.hEvent = connectEvent;

    BOOL connected = ConnectNamedPipe(m_hPipe, &overlapped);
    DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && error == ERROR_IO_PENDING) {
        HANDLE waits[] = { m_hShutdownEvent, connectEvent };
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE,
                                                  timeoutMs);
        if (wait == WAIT_OBJECT_0 + 1) {
            DWORD ignored = 0;
            connected = GetOverlappedResult(m_hPipe, &overlapped, &ignored, FALSE);
            error = connected ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelAndReap(m_hPipe, overlapped);
            error = wait == WAIT_TIMEOUT ? ERROR_SEM_TIMEOUT : ERROR_OPERATION_ABORTED;
            connected = FALSE;
        }
    } else if (!connected && error == ERROR_PIPE_CONNECTED) {
        // The client opened the instance between CreateNamedPipeW and
        // ConnectNamedPipe. This is a completed connection, not an error.
        connected = TRUE;
        error = ERROR_SUCCESS;
    } else if (connected) {
        DWORD ignored = 0;
        connected = GetOverlappedResult(m_hPipe, &overlapped, &ignored, FALSE);
        error = connected ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(connectEvent);

    if (!connected) {
        if (m_shutdownRequested.load() || error == ERROR_OPERATION_ABORTED) {
            FACELOGIN_INFO(L"Pipe wait cancelled by service stop request");
        } else if (error != ERROR_SEM_TIMEOUT) {
            FACELOGIN_WARN(L"Client connection failed: %lu", error);
        }
        Close();
        return false;
    }
    if (m_shutdownRequested.load()) {
        FACELOGIN_INFO(L"Pipe wait cancelled by service stop request");
        Close();
        return false;
    }

    m_connected = true;
    FACELOGIN_DEBUG(L"Client connected");
    return true;
}

bool PipeServer::ReadMessage(std::wstring& outMessage, DWORD timeoutMs) {
    if (!m_connected || m_hPipe == INVALID_HANDLE_VALUE || !m_hShutdownEvent ||
        m_shutdownRequested.load()) return false;

    wchar_t buffer[ipc::PIPE_BUFFER_SIZE / sizeof(wchar_t)] = {};
    HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readEvent) {
        FACELOGIN_ERROR(L"CreateEvent for pipe read failed: %lu", GetLastError());
        return false;
    }
    OVERLAPPED overlapped = {};
    overlapped.hEvent = readEvent;
    DWORD bytesRead = 0;
    BOOL result = ReadFile(m_hPipe, buffer,
                           static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)),
                           nullptr, &overlapped);
    DWORD error = result ? ERROR_SUCCESS : GetLastError();
    if (!result && error == ERROR_IO_PENDING) {
        HANDLE waits[] = { m_hShutdownEvent, readEvent };
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE,
                                                  timeoutMs);
        if (wait == WAIT_OBJECT_0 + 1) {
            result = GetOverlappedResult(m_hPipe, &overlapped, &bytesRead, FALSE);
            error = result ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelAndReap(m_hPipe, overlapped);
            error = wait == WAIT_TIMEOUT ? ERROR_SEM_TIMEOUT : ERROR_OPERATION_ABORTED;
            result = FALSE;
        }
    } else if (result) {
        result = GetOverlappedResult(m_hPipe, &overlapped, &bytesRead, FALSE);
        error = result ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(readEvent);

    if (!result || bytesRead == 0) {
        if (error == ERROR_SEM_TIMEOUT) {
            FACELOGIN_WARN(L"ReadMessage timed out after %lu ms with no data", timeoutMs);
        } else if (m_shutdownRequested.load() || error == ERROR_OPERATION_ABORTED) {
            FACELOGIN_INFO(L"Pipe read cancelled by service stop request");
        } else if (IsPipeClosedError(error)) {
            FACELOGIN_INFO(L"Pipe broken by client");
            m_connected = false;
        } else if (error == ERROR_MORE_DATA) {
            // Message larger than the fixed buffer; the protocol has no
            // fragmentation. Fail closed and let the caller disconnect.
            FACELOGIN_ERROR(L"ReadMessage: message exceeds %u bytes; "
                            L"closing connection", ipc::PIPE_BUFFER_SIZE);
            m_connected = false;
        } else {
            FACELOGIN_ERROR(L"Overlapped ReadFile failed: %lu", error);
            m_connected = false;
        }
        SecureZeroMemory(buffer, sizeof(buffer));
        return false;
    }

    size_t len = bytesRead / sizeof(wchar_t);
    // Strip trailing null terminator(s) added by pipe WriteFile
    while (len > 0 && buffer[len - 1] == L'\0') {
        len--;
    }
    outMessage.assign(buffer, len);
    SecureZeroMemory(buffer, sizeof(buffer));
    return true;
}

bool PipeServer::WriteMessage(const std::wstring& message) {
    if (m_shutdownRequested.load() || !m_connected ||
        m_hPipe == INVALID_HANDLE_VALUE || !m_hShutdownEvent) return false;

    DWORD byteSize = static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t));
    HANDLE writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!writeEvent) {
        FACELOGIN_ERROR(L"CreateEvent for pipe write failed: %lu", GetLastError());
        return false;
    }
    OVERLAPPED overlapped = {};
    overlapped.hEvent = writeEvent;
    DWORD bytesWritten = 0;
    BOOL result = WriteFile(m_hPipe, message.c_str(), byteSize,
                            nullptr, &overlapped);
    DWORD error = result ? ERROR_SUCCESS : GetLastError();
    if (!result && error == ERROR_IO_PENDING) {
        HANDLE waits[] = { m_hShutdownEvent, writeEvent };
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE,
                                                  kPipeWriteTimeoutMs);
        if (wait == WAIT_OBJECT_0 + 1) {
            result = GetOverlappedResult(m_hPipe, &overlapped,
                                         &bytesWritten, FALSE);
            error = result ? ERROR_SUCCESS : GetLastError();
        } else {
            CancelAndReap(m_hPipe, overlapped);
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
            result = FALSE;
        }
    } else if (result) {
        result = GetOverlappedResult(m_hPipe, &overlapped,
                                     &bytesWritten, FALSE);
        error = result ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(writeEvent);

    if (!result || bytesWritten != byteSize) {
        if (IsPipeClosedError(error)) {
            FACELOGIN_INFO(L"Pipe broken during write");
        } else if (error == ERROR_TIMEOUT) {
            FACELOGIN_WARN(L"Pipe write timed out after %lu ms", kPipeWriteTimeoutMs);
        } else if (m_shutdownRequested.load() || error == ERROR_OPERATION_ABORTED) {
            FACELOGIN_INFO(L"Pipe write cancelled by service stop request");
        } else {
            FACELOGIN_ERROR(L"Overlapped WriteFile failed: %lu", error);
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
    if (m_hPipe != INVALID_HANDLE_VALUE && m_connected) {
        // Delivery guarantees belong to the explicit bounded ACK protocol.
        // FlushFileBuffers on a server pipe can wait forever for a broken or
        // non-reading client and must never be used as a disconnect primitive.
        DisconnectNamedPipe(m_hPipe);
        m_connected = false;
    }
}

void PipeServer::RequestShutdown() {
    m_shutdownRequested.store(true);
    if (m_hShutdownEvent) SetEvent(m_hShutdownEvent);
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
