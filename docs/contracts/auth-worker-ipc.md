# 认证 worker 私有 IPC 契约

本文是 `FaceLoginService.exe` 父服务与同一 EXE 的 `-auth-worker` 模式之间的线协议事实源。它不替代 Credential Provider 使用的公共命名管道；公共协议见 [`ipc.md`](ipc.md)。实现位于 `face_service/auth_worker_protocol.*`，父端状态机位于 `auth_worker_client.*`。

## 通道与安全边界

- 父进程创建两根匿名管道：父→子和子→父；不创建新的公共命名管道。
- `CreateProcessW` 使用绝对 EXE 路径、完整引号、`CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT`。
- `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 只允许继承两只 worker 管道句柄。父进程先把子进程加入带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job，再恢复主线程。
- worker 必须以 LocalSystem 运行，并在取得有效继承句柄、完成 `HELLO → INIT → READY` 握手后才进入就绪态；任一条件不成立立即退出。
- 通道只承载配置、状态文本、三条 512-D embedding 和判定结果。用户名、SID、密码、DPAPI 密文、`users.dat` 记录及完整 `AUTH_SUCCESS` 永不进入 worker 通道。
- worker 仍位于服务的 Session 0；该协议不授予它进入交互用户 Session 的能力。

## 固定头

所有整数和 `float` 使用 Windows x64 原生小端表示。头按 1 字节打包，固定 20 字节：

```cpp
struct MessageHeader {
    uint32_t magic;       // 0x4B574C46，即 "FLWK"
    uint16_t version;     // 5
    uint16_t type;
    uint64_t requestId;
    uint32_t payloadSize;
};
```

`payloadSize` 上限为 16 KiB。错误 magic/version、0 或未知类型、超长/截断消息、payload 尾随字节以及读写错误均使本次 worker fail-closed；父进程随后关闭 Job，不尝试退回父进程内认证。

## 消息类型

| 值 | 类型 | 方向 | requestId | payload |
|---:|---|---|---|---|
| 1 | `Hello` | 子→父 | 0 | 空 |
| 2 | `Init` | 父→子 | 0 | `WorkerConfig` |
| 3 | `Ready` | 子→父 | 0 | 空 |
| 4 | `StartAuth` | 父→子 | 非 0 | 空 |
| 5 | `Status` | 子→父 | 当前请求 | UTF-16 字符串 |
| 6 | `MatchProbe` | 子→父 | 当前请求 | binding index + embedding |
| 7 | `MatchAccept` | 父→子 | 当前请求 | 空 |
| 8 | `MatchRetry` | 父→子 | 当前请求 | 空 |
| 9 | `MatchReject` | 父→子 | 当前请求 | UTF-16 错误文本 |
| 10 | `AuthSucceeded` | 子→父 | 当前请求 | `AuthTiming` |
| 11 | `AuthFailed` | 子→父 | 当前请求 | UTF-16 错误文本 |
| 12 | `AuthTimedOut` | 子→父 | 当前请求 | 空 |
| 13 | `Fatal` | 子→父 | 0（启动阶段） | UTF-16 错误文本 |
| 14 | `Cancel` | 父→子 | 0 | 空 |

设计中的 `MATCH_DECISION` 在 v1 线上拆为 `MatchAccept/MatchRetry/MatchReject`，`AUTH_COMPLETE` 拆为 `AuthSucceeded/AuthFailed/AuthTimedOut`，使每个终态的合法 payload 可单独校验。`Cancel` 是父端停止、解锁、服务退出或客户端断开时的协议意图；父端同时终止 Job，因此不会等待被驱动或推理阻塞的 worker 自愿退出。

## Payload 编码

字符串编码为 `uint32_t charCount`，随后是恰好 `charCount` 个 UTF-16LE `wchar_t`；不带 NUL。通用字符串上限为 2048 个字符，解码后必须正好耗尽 payload。

`Init` 的 `WorkerConfig` 顺序固定为：

```text
int32  cameraRotation      // 0/90/180/270
float  antiSpoofThreshold // 有限值，[0.15, 0.50]
int32  authTimeoutSeconds // [1, 60]
uint32 lowLightEnhance     // 0/1
wstring cameraDevice
```

`MatchProbe` 的 payload 为（v4 起附带范数，v5 起附带姿态角）：

```text
uint32 bindingIndex       // 线上严格为 0 → 1 → 2；日志显示为 1/3 → 2/3 → 3/3
uint32 embeddingCount     // 必须恰好 512
float  embedding[512]
float  preNorm            // 归一化前的识别器输出范数（质量信号，必须有限且 >0）
float  yawDeg             // 帧姿态 yaw 估计（度，有限且 |v| ≤ 90）    ← v5
float  pitchDeg           // 帧姿态 pitch 估计（同上）                 ← v5
```

每个 embedding 元素必须 `isfinite`，L2 范数必须处于 `[0.90, 1.10]`；`preNorm` 供父端渐进学习的范数门使用（w600k_r50 量级 ≈20–25，嵌入本身以归一化形态传输、范数不可从向量恢复）；`yawDeg`/`pitchDeg` 由 worker 侧 `EstimateYawDeg`/`EstimatePitchDeg`（`face_align.h` 弱透视模型，与录入同源）从绑定帧关键点估计，供父端学习姿态锥门使用（`docs/progressive-learning-v2.md` §3）。父端只在本地调用 `FindBestIdentity`，worker 只收到 accept/retry/reject，不知道匹配到的 SID。

`AuthSucceeded` 的 `AuthTiming` 顺序固定为：

```text
float cameraInitMs
float pipelineMs
float totalMs
```

三个值必须有限、非负且不超过 120000 ms，`totalMs` 必须在 10 ms 误差内等于前两项之和。计时只用于诊断，不参与认证判定。worker 在成功关键路径不写同步计时日志，而是先发送此终态；父服务严格解码，并在公共 `AUTH_SUCCESS` 已写入后记录计时。空、截断、尾随字节、NaN、Inf、负数、超界或总计不一致均使本次认证 fail-closed。

## 认证状态机

```text
spawn suspended → assign Job → resume
  子 Hello(0)
  父 Init(0)
  子加载四模型；默认不打开相机
  子 Ready(0)
  父 StartAuth(requestId)
  子按需打开相机并运行 PAD 5/5
  子 MatchProbe(0) ↔ 父 decision
  子 MatchProbe(1) ↔ 父 decision
  子 MatchProbe(2) ↔ 父 decision
  子 terminal(requestId)
  父关闭/终止 Job并确认进程退出
  父仅在成功且三次 identity 为同一 SID 后解密一次密码
```

父端对一个认证请求维护独立 `AuthExchangeValidator`：所有活动消息必须使用当前 request ID；binding 不得重复、跳号或缺失；只有三个 probe 都被接受后 `AuthSucceeded` 才合法。任何不同 SID 由父端返回 `MatchReject`。成功后若 SID 为空、数据库已重载或记录消失，最终凭据加载仍 fail-closed。

worker 每次最多处理一个 `StartAuth`。成功、失败或超时终态写入管道后进程立即退出，进程退出是 DirectShow、ORT Session、线程池和 Windows 堆高水位的确定性回收边界。

## 验证入口

`AuthWorkerProtocolTest` 覆盖消息往返、超长/截断、magic/version/type、配置边界、512 维、NaN/Inf/异常范数、binding 越序/重复/缺失和 request ID 不一致。`AuthWorkerLifecycleTest` 使用同一父端 supervisor 的 mock worker 覆盖启动失败、模型失败、崩溃、超时、断管、取消、100 次创建/认证/退出以及父进程退出时 Job 杀死子进程。
