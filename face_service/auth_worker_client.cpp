#include "auth_worker_client.h"

#include "timer_resolution.h"

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace facelogin {
namespace {

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::wstring(path.data(), length);
}

std::wstring WorkerCommandLine(const std::wstring& executable,
                               HANDLE parentToWorker, HANDLE workerToParent) {
    return L"\"" + executable + L"\" -auth-worker --in " +
        std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(parentToWorker))) +
        L" --out " +
        std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(workerToParent)));
}

bool IsClosedRead(auth_worker::ReadStatus status) {
    return status == auth_worker::ReadStatus::Closed ||
           status == auth_worker::ReadStatus::IoError ||
           status == auth_worker::ReadStatus::Invalid;
}

} // namespace

AuthWorkerClient::AuthWorkerClient(auth_worker::WorkerConfig config,
                                   DWORD authSupervisorTimeoutMs,
                                   std::wstring executablePathOverride)
    : m_config(std::move(config)),
      m_authSupervisorTimeoutMs(authSupervisorTimeoutMs == 0
          ? 20000 : authSupervisorTimeoutMs),
      m_executablePathOverride(std::move(executablePathOverride)) {}

AuthWorkerClient::~AuthWorkerClient() {
    Stop();
}

bool AuthWorkerClient::IsReady() const {
    return m_ready && m_channel.IsValid() && IsAlive();
}

bool AuthWorkerClient::IsAlive() const {
    return m_process && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

void AuthWorkerClient::CloseProcessHandles() {
    if (m_process) {
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_job) {
        CloseHandle(m_job);  // KILL_ON_JOB_CLOSE is the final orphan guard.
        m_job = nullptr;
    }
    m_ready = false;
}

void AuthWorkerClient::CompleteTerminal() {
    // The terminal frame ends this one-shot worker's ownership. Reuse Stop's
    // terminate-Job-then-wait ordering so every successful, failed and timed
    // out exchange returns with the resource boundary fully reclaimed.
    Stop();
}

void AuthWorkerClient::Stop() {
    m_ready = false;
    if (m_channel.IsValid()) {
        m_channel.Write(auth_worker::MessageType::Cancel, 0);
    }
    if (m_job) {
        // Do not wait for a blocked camera driver or ONNX call. Process exit is
        // the resource-reclamation mechanism, so force the Job closed now.
        TerminateJobObject(m_job, ERROR_CANCELLED);
    }
    if (m_process) WaitForSingleObject(m_process, 2000);
    m_channel.Close();
    CloseProcessHandles();
}

bool AuthWorkerClient::Spawn(std::wstring& errorMessage) {
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE childRead = nullptr;
    HANDLE parentWrite = nullptr;
    HANDLE parentRead = nullptr;
    HANDLE childWrite = nullptr;
    if (!CreatePipe(&childRead, &parentWrite, &inheritable, 0) ||
        !SetHandleInformation(parentWrite, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&parentRead, &childWrite, &inheritable, 0) ||
        !SetHandleInformation(parentRead, HANDLE_FLAG_INHERIT, 0)) {
        errorMessage = L"无法创建认证工作进程通信通道";
        if (childRead) CloseHandle(childRead);
        if (parentWrite) CloseHandle(parentWrite);
        if (parentRead) CloseHandle(parentRead);
        if (childWrite) CloseHandle(childWrite);
        return false;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<BYTE> attributeBuffer(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
        errorMessage = L"无法初始化工作进程安全属性";
        CloseHandle(childRead); CloseHandle(parentWrite);
        CloseHandle(parentRead); CloseHandle(childWrite);
        return false;
    }
    HANDLE inheritedHandles[] = { childRead, childWrite };
    const bool handleListOk = UpdateProcThreadAttribute(
        attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles,
        sizeof(inheritedHandles), nullptr, nullptr) != FALSE;
    if (!handleListOk) {
        DeleteProcThreadAttributeList(attributes);
        errorMessage = L"无法限制工作进程继承句柄";
        CloseHandle(childRead); CloseHandle(parentWrite);
        CloseHandle(parentRead); CloseHandle(childWrite);
        return false;
    }

    const std::wstring executable = m_executablePathOverride.empty()
        ? CurrentExecutablePath() : m_executablePathOverride;
    const std::wstring commandLine = executable.empty()
        ? std::wstring{} : WorkerCommandLine(executable, childRead, childWrite);
    if (commandLine.empty()) {
        DeleteProcThreadAttributeList(attributes);
        errorMessage = L"无法解析认证工作进程路径";
        CloseHandle(childRead); CloseHandle(parentWrite);
        CloseHandle(parentRead); CloseHandle(childWrite);
        return false;
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION processInfo{};
    const DWORD creationFlags = CREATE_SUSPENDED | CREATE_NO_WINDOW |
                                EXTENDED_STARTUPINFO_PRESENT;
    const bool created = CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr,
                                        TRUE, creationFlags, nullptr, nullptr,
                                        &startup.StartupInfo, &processInfo) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(childRead);
    CloseHandle(childWrite);
    if (!created) {
        errorMessage = L"无法启动认证工作进程";
        CloseHandle(parentWrite);
        CloseHandle(parentRead);
        return false;
    }

    m_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m_job || !SetInformationJobObject(m_job, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(m_job, processInfo.hProcess)) {
        errorMessage = L"无法将认证工作进程纳入监控作业";
        TerminateProcess(processInfo.hProcess, ERROR_ACCESS_DENIED);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        CloseHandle(parentWrite);
        CloseHandle(parentRead);
        if (m_job) { CloseHandle(m_job); m_job = nullptr; }
        return false;
    }

    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        errorMessage = L"无法恢复认证工作进程";
        TerminateJobObject(m_job, ERROR_ACCESS_DENIED);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        CloseHandle(parentWrite);
        CloseHandle(parentRead);
        CloseHandle(m_job);
        m_job = nullptr;
        return false;
    }
    CloseHandle(processInfo.hThread);
    m_process = processInfo.hProcess;
    m_channel = auth_worker::Channel(parentRead, parentWrite);
    return true;
}

bool AuthWorkerClient::ReadStartupMessage(auth_worker::MessageType expected,
                                          DWORD timeoutMs,
                                          std::wstring& errorMessage) {
    auth_worker::Message message;
    const auth_worker::ReadStatus status = m_channel.Read(message, timeoutMs);
    if (status != auth_worker::ReadStatus::Ok) {
        errorMessage = L"认证工作进程启动超时或中断";
        return false;
    }
    if (message.type == auth_worker::MessageType::Fatal) {
        if (!auth_worker::DecodeWString(message.payload, errorMessage)) {
            errorMessage = L"认证工作进程初始化失败";
        }
        return false;
    }
    if (message.type != expected || message.requestId != 0 || !message.payload.empty()) {
        errorMessage = L"认证工作进程启动协议异常";
        return false;
    }
    return true;
}

bool AuthWorkerClient::Start(std::wstring& errorMessage) {
    Stop();
    if (!Spawn(errorMessage)) return false;
    if (!ReadStartupMessage(auth_worker::MessageType::Hello, 5000, errorMessage) ||
        !m_channel.Write(auth_worker::MessageType::Init, 0,
                         auth_worker::EncodeConfig(m_config)) ||
        !ReadStartupMessage(auth_worker::MessageType::Ready, 15000, errorMessage)) {
        Stop();
        return false;
    }
    m_ready = true;
    return true;
}

AuthWorkerResult AuthWorkerClient::Authenticate(AuthWorkerCallbacks callbacks) {
    AuthWorkerResult result;
    if (!IsReady() || !callbacks.reportStatus || !callbacks.isCancelled ||
        !callbacks.verifyBinding) {
        result.errorMessage = L"认证工作进程不可用";
        return result;
    }

    const uint64_t requestId = m_nextRequestId++;
    const auto authStart = std::chrono::steady_clock::now();
    // This loop picks the worker's messages up via PeekNamedPipe+Sleep(1)
    // polling; on the default ~15.6 ms clock tick that Sleep actually waits a
    // full tick (measured 2026-09-03: every pickup averages half a tick and
    // the terminal waits a whole one, ~40 ms total on the dev machine). Raise
    // the timer resolution for the authentication window; the guard releases
    // on every return path. The preloaded worker raises its own — see
    // RunAuthenticationWorker.
    const ScopedTimerResolution fineTimer(1);
    if (!m_channel.Write(auth_worker::MessageType::StartAuth, requestId)) {
        result.errorMessage = L"认证工作进程通信失败";
        Stop();
        return result;
    }

    auth_worker::AuthExchangeValidator exchange(requestId);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(m_authSupervisorTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (callbacks.isCancelled()) {
            result.cancelled = true;
            Stop();
            return result;
        }
        auth_worker::Message message;
        // Active authentication has only a handful of short private messages.
        // Poll at 1 ms here to avoid adding a 5 ms quantum to every status,
        // binding and terminal frame; the idle preloaded worker keeps the
        // default 5 ms interval.
        const auth_worker::ReadStatus read = m_channel.Read(message, 200, 1);
        if (read == auth_worker::ReadStatus::Timeout) continue;
        if (IsClosedRead(read) || !exchange.HasExpectedRequestId(message.requestId)) {
            result.errorMessage = L"认证工作进程中途退出";
            Stop();
            return result;
        }

        if (message.type == auth_worker::MessageType::Status) {
            std::wstring status;
            if (!auth_worker::DecodeWString(message.payload, status)) {
                result.errorMessage = L"认证工作进程状态协议异常";
                Stop();
                return result;
            }
            callbacks.reportStatus(status);
            continue;
        }

        if (message.type == auth_worker::MessageType::MatchProbe) {
            unsigned int bindingIndex = 0;
            std::vector<float> embedding;
            float preNorm = 0.0f;
            if (!auth_worker::DecodeMatchProbe(message.payload, bindingIndex, embedding,
                                               preNorm) ||
                !exchange.IsExpectedProbe(bindingIndex)) {
                result.errorMessage = L"认证工作进程身份绑定协议异常";
                Stop();
                return result;
            }
            const BindingDecision decision =
                callbacks.verifyBinding(embedding, bindingIndex, preNorm);
            auth_worker::MessageType response = auth_worker::MessageType::MatchRetry;
            std::vector<uint8_t> payload;
            if (decision.kind == BindingDecisionKind::Accept) {
                response = auth_worker::MessageType::MatchAccept;
                if (!exchange.AcceptProbe(bindingIndex)) {
                    result.errorMessage = L"认证工作进程身份绑定协议异常";
                    Stop();
                    return result;
                }
            } else if (decision.kind == BindingDecisionKind::Reject) {
                response = auth_worker::MessageType::MatchReject;
                payload = auth_worker::EncodeWString(decision.errorMessage);
            }
            if (!m_channel.Write(response, requestId, payload)) {
                result.errorMessage = L"认证工作进程通信失败";
                Stop();
                return result;
            }
            continue;
        }

        if (message.type == auth_worker::MessageType::AuthSucceeded &&
            exchange.CanSucceed()) {
            auth_worker::AuthTiming timing;
            if (!auth_worker::DecodeAuthTiming(message.payload, timing)) {
                result.errorMessage = L"认证工作进程计时协议异常";
                Stop();
                return result;
            }
            result.succeeded = true;
            result.hasTiming = true;
            result.timing = timing;
            result.supervisorElapsedMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - authStart).count();
            // The worker self-terminated before this message arrived
            // (SendTerminalAndExit), so no worker code can run again —
            // reclaiming the Job only waits for kernel process teardown
            // (0–2 ms dev machine, 15–40 ms measured on the slow machine).
            // That wait must not sit between the match verdict and the
            // AUTH_SUCCESS delivery, so this path returns without reaping:
            // the caller owns the final Stop() (FaceService reaps right
            // after the Credential Provider acknowledges the credentials)
            // and the shared_ptr destructor backstops every other exit.
            result.cleanupMs = 0.0;
            return result;
        }
        if (message.type == auth_worker::MessageType::AuthTimedOut &&
            message.payload.empty()) {
            result.timedOut = true;
            CompleteTerminal();
            return result;
        }
        if (message.type == auth_worker::MessageType::AuthFailed ||
            message.type == auth_worker::MessageType::Fatal) {
            if (!auth_worker::DecodeWString(message.payload, result.errorMessage)) {
                result.errorMessage = L"认证工作进程执行失败";
            }
            CompleteTerminal();
            return result;
        }

        result.errorMessage = L"认证工作进程返回了无效消息";
        Stop();
        return result;
    }

    result.timedOut = true;
    Stop();
    return result;
}

} // namespace facelogin
