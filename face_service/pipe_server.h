#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <accctrl.h>
#include <aclapi.h>

namespace facelogin {

// Named pipe server for communication with the credential provider DLL.
// Uses OVERLAPPED connect/read/write operations. Each pending operation waits
// on its completion event together with the service shutdown event, so data,
// timeout and cancellation are immediate without polling.
// Security: SYSTEM + Administrators + current interactive user can connect;
// PIPE_REJECT_REMOTE_CLIENTS rejects remote clients at the transport level.

class PipeServer {
public:
    PipeServer();
    ~PipeServer();

    // Non-copyable
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Create the named pipe and wait for a client connection. The event-driven
    // wait is bounded by timeoutMs and observes RequestShutdown() immediately.
    // pipeName overrides ipc::PIPE_NAME (dev/test tools only, to avoid
    // colliding with a live service's single instance); nullptr = production
    // name. Returns true when a client has connected.
    bool WaitForClient(DWORD timeoutMs = 30000,
                       const wchar_t* pipeName = nullptr);

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

    // Read a null-terminated UTF-16LE message from the pipe. Returns true and
    // sets outMessage on success. The OVERLAPPED wait is bounded by timeoutMs
    // and can be cancelled by RequestShutdown().
    bool ReadMessage(std::wstring& outMessage, DWORD timeoutMs = 30000);

    // Write a null-terminated UTF-16LE message and wait for the bounded
    // OVERLAPPED operation to complete.
    bool WriteMessage(const std::wstring& message);

    // Disconnect current client (allows a new client to connect).
    void Disconnect();

    // Tell a waiting server loop to exit. This is safe from the SCM control
    // handler: it does not touch the pipe HANDLE owned by the service thread.
    void RequestShutdown();

    // Close the pipe entirely. Call only after the server loop has stopped.
    void Close();

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
    HANDLE m_hShutdownEvent = nullptr;
    bool m_connected = false;
    std::atomic<bool> m_shutdownRequested{false};
};

} // namespace facelogin
