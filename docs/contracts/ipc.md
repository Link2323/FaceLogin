# IPC 契约

本文定义 FaceLogin 运行期命名管道的兼容边界。实现事实源是 [`common/ipc_protocol.h`](../../common/ipc_protocol.h)、[`common/ipc_protocol.cpp`](../../common/ipc_protocol.cpp)、[`face_service/pipe_server.cpp`](../../face_service/pipe_server.cpp) 和各客户端读写代码；本文用于解释跨文件约束。

## 参与方

- 服务端：`FaceLoginService.exe`
- 认证客户端：LogonUI 中加载的 `FaceLoginCredentialProvider.dll`
- 管理客户端：`FaceLoginConsole.exe`，当前通过管道发送 `RELOAD_DB` / `CONFIG_RELOAD` / `LEARNING_STATUS`

安装器不参与运行期 IPC。注册控制台的服务日志页面直接读取 `<DataPath>\log\service.log`，以避开固定消息缓冲区限制。

## 传输层

| 项目 | 当前契约 |
|---|---|
| 管道名 | `\\.\pipe\FaceLoginPipe` |
| 类型 | `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT` |
| 实例数 | 1，串行处理客户端 |
| 编码 | Windows `wchar_t`，即 UTF-16LE |
| 分帧 | 使用命名管道消息边界，无长度前缀 |
| 服务端缓冲区 | `ipc::PIPE_BUFFER_SIZE = 4096` 字节 |
| 远程访问 | `PIPE_REJECT_REMOTE_CLIENTS` |

服务端的 `ConnectNamedPipe` / `ReadFile` / `WriteFile` 与 Credential Provider
的 `ReadFile` / `WriteFile` 均使用 overlapped I/O。完成事件与停止事件同时
等待；超时或停止时必须 `CancelIoEx`，并在释放 `OVERLAPPED`、事件和缓冲区
前用 `GetOverlappedResult` 回收完成状态。`PIPE_WAIT` 仍表示消息管道的阻塞
语义，不代表实现通过 sleep/peek 轮询。

通用客户端和服务端写入时包含结尾 `NUL`，接收端会剥离尾随 `NUL`。注册控制台的部分单向通知按 `msg.size() * sizeof(wchar_t)` 写入，不携带 `NUL`，因此任何接收端都不得把 `NUL` 当作唯一消息边界。

当前没有分片/重组层。新增大载荷前必须同时检查服务端、Credential Provider 和注册控制台的固定缓冲区及 `ERROR_MORE_DATA` 路径。

## 访问控制

- 服务模式下有效主体为 SYSTEM + Administrators。
- standalone 模式会尝试额外加入当前运行用户。
- 管道拒绝远程客户端。
- 明文密码只存在于认证成功消息和短生命周期内存中；禁止记录消息全文，使用后必须清零。

## 消息表

| 消息 | 方向 | 当前行为 |
|---|---|---|
| `AUTH_REQUEST` | CP → 服务 | 发起一次认证 |
| `STATUS:text` | 服务 → CP | 推送非终态 UI 文本 |
| `AUTH_SUCCESS:...` | 服务 → CP | 返回认证凭据，格式见下 |
| `AUTH_TIMEOUT` | 服务 → CP | 全局认证时限耗尽 |
| `AUTH_ERROR:message` | 服务 → CP | 无人脸、PAD 失败、模型异常、无注册用户等终态错误 |
| `AUTH_ACK` | CP → 服务 | CP 已复制并擦除终态传输缓冲区；服务最多等待 2 秒 |
| `RELOAD_DB` / `RELOAD_OK` | 控制台 ↔ 服务 | 凭据数据库保存后热重载；客户端必须保持双向连接并等到 `RELOAD_OK`，不能写完即关闭 |
| `CONFIG_RELOAD` / `CONFIG_RELOAD_OK` / `CONFIG_RELOAD_ERROR` | 控制台 ↔ 服务 | 配置热重载；无效活体配置保持 fail-closed |
| `LEARNING_STATUS` | 控制台 ↔ 服务 | 只读查询渐进式学习状态；响应为单条 JSON（`enabled`/`gate_cap`/`faces[]`/`recent[]`，无 OK/ERROR 后缀），服务端同步组装。数据为内存快照，服务重启清零（持久事实源是日志）。客户端读取后回 `CONTROL_ACK`。管道单实例，控制台侧按 10s 节流，不得变成高频轮询 |
| `CONTROL_ACK` | 控制台 → 服务 | 控制台已读取 reload 响应；服务最多等待 2 秒 |

## `AUTH_SUCCESS` 当前格式

```text
AUTH_SUCCESS:SID:UPN:DOMAIN\USER:PASSWORD
```

解析规则：

1. 去掉 `AUTH_SUCCESS:` 前缀。
2. 按载荷中的前三个冒号切分 `SID`、`UPN`、账户字段。
3. 第三个冒号后的全部字符都是密码，因此密码可以包含冒号。
4. SID、UPN、域名和用户名不得包含冒号。
5. UPN 可为空，本地账户仍保留 `SID::DOMAIN\USER:PASSWORD` 的两个连续冒号。
6. 消息不包含 `faceId`，也没有独立协议版本字段。

旧格式 `AUTH_SUCCESS:DOMAIN\USER:PASSWORD` 不被接受；服务与 Credential Provider 必须使用同一版本安装。

## 典型认证序列

```text
CP                    Service
 |--- AUTH_REQUEST ---->|
 |<-- STATUS:... -------|  0..N 次
 |<-- AUTH_SUCCESS:... -|  成功终态
 |        或             |
 |<-- AUTH_TIMEOUT ------|  超时终态
 |<-- AUTH_ERROR:... ----|  其他失败终态
 |--- AUTH_ACK --------->|  已复制终态（2 秒截止）
```

终态消息后连接结束。CP 的后台线程用 I/O 完成事件等待消息，先擦除固定传输缓冲区并发送 `AUTH_ACK`，再交付终态回调；ACK 失败不丢弃已经收到的终态，服务在 2 秒截止后自行断开。服务端禁止用 `FlushFileBuffers` 或 `PeekNamedPipe` 猜测反向输出是否被消费。不要把管道读取移回 COM 主线程。

## 修改检查表

修改消息、缓冲区或认证字段时同时检查：

- `common/ipc_protocol.h/cpp`
- `face_service/FaceService.cpp`
- `face_service/pipe_server.*`
- `credential_provider/pipe_client.*`
- `credential_provider/FaceLoginCredential.*`
- `enrollment_app/EnrollmentWizard.*`
- 本文及 `DEVELOPMENT.md` 的路由表

至少验证：当前格式解析、含冒号密码、空 UPN、旧格式拒绝、终态后断开、客户端中途断开、超长消息处理，以及日志中没有明文密码。
