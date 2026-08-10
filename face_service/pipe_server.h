#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <accctrl.h>
#include <aclapi.h>

namespace facelogin {

// Named pipe server for communication with the credential provider DLL.
// Uses synchronous I/O (no FILE_FLAG_OVERLAPPED) for reliability.
// Security: SYSTEM + Administrators + current interactive user can connect.

class PipeServer {
public:
    PipeServer() = default;
    ~PipeServer();

    // Non-copyable
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Create the named pipe and wait for a client connection.
    // Blocks until a client connects or the handle is closed (via Close()).
    // Returns true when a client has connected.
    bool WaitForClient(DWORD timeoutMs = 30000);

    // Create the named pipe instance (security descriptor + handle) WITHOUT
    // blocking for a client. Separated from WaitForClient so the create→close
    // cycle can be driven directly (dev tool tools/pipe_lifecycle).
    // pipeName overrides ipc::PIPE_NAME (used by the dev tool to avoid
    // colliding with a live service's single instance); nullptr = production
    // name.
    bool CreatePipeInstance(DWORD timeoutMs = 30000,
                            const wchar_t* pipeName = nullptr);

    // Number of SetEntriesInAclW allocations not yet LocalFree'd. Dev/test
    // aid for the per-instance ACL ownership fix (tools/pipe_lifecycle): an
    // absolute-format security descriptor does NOT own its DACL, so one ACL
    // used to leak per pipe instance. Must read 0 after every Close().
    static long OutstandingAclAllocations() { return g_aclAllocations.load(); }

    // Read a null-terminated UTF-16LE message from the pipe (synchronous).
    // Returns true and sets outMessage on success. Honors timeoutMs by polling
    // PeekNamedPipe so the caller can never block indefinitely.
    bool ReadMessage(std::wstring& outMessage, DWORD timeoutMs = 30000);

    // Write a null-terminated UTF-16LE message to the pipe (synchronous).
    bool WriteMessage(const std::wstring& message);

    // Wait (bounded) until the client has consumed pending output and the
    // pipe is idle — i.e. no more bytes remain to be read. This replaces the
    // unbounded ReadFile(dummy) handshake: it never blocks forever, and
    // returns immediately if the client has already closed its end.
    // Returns true if the pipe drained (or the client closed); false on
    // timeout.
    bool DrainOutput(DWORD timeoutMs = 5000);

    // Disconnect current client (allows a new client to connect).
    void Disconnect();

    // Close the pipe entirely. Unblocks any pending I/O.
    void Close();

    // Get the raw pipe handle (for FlushFileBuffers, etc.)
    HANDLE GetHandle() const { return m_hPipe; }

    bool IsConnected() const { return m_connected; }

    // Non-blocking: returns true if the connected client has closed its end
    // of the pipe (or the pipe is otherwise broken). Uses PeekNamedPipe so it
    // never blocks — safe to poll from a busy authentication loop.
    bool IsClientDisconnected() const;

private:
    // Absolute-format security descriptor + its separately-allocated DACL.
    // An absolute SD does NOT own its DACL — both must be LocalFree'd once
    // CreateNamedPipeW has copied the descriptor into the new pipe instance.
    // The destructor does both frees and keeps g_aclAllocations in sync, so
    // there is exactly one ownership path.
    struct PipeSecurity {
        PSECURITY_DESCRIPTOR sd = nullptr;
        PACL acl = nullptr;
        ~PipeSecurity();
    };

    bool CreateSecurityDescriptor(PipeSecurity& out);

    static std::atomic<long> g_aclAllocations;

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
    bool m_connected = false;
};

} // namespace facelogin
