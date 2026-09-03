# 认证 Worker 迁移完成与验收报告

> 状态：**已完成**  
> 验收日期：2026-08-13  
> 适用版本：当前 worker 隔离工作树及最终安装器候选  
> 私有协议契约：[`contracts/auth-worker-ipc.md`](contracts/auth-worker-ipc.md)

## 1. 结论

FaceLogin 的生产认证已从长期父服务内执行，迁移为 **LocalSystem / Session 0 中的一次性认证 worker**。父服务不再持有 DirectShow、ONNX Runtime Session 或 ORT 全局线程池；worker 在完成一次认证或发生失败、取消、超时后退出，由进程退出确定性回收摄像头驱动句柄和 Windows 堆高水位。

最终结论：

- `worker + DirectShow` 是生产方案，性能和长期资源门槛均通过。
- 历史 Session 0 对比中另一条实验相机路径整体更慢；该实现与 A/B 配置已删除，产品只保留 DirectShow。
- PAD 仍为 5/5，身份仍在第 1/3/5 个有效帧绑定三次，阈值、超时和公共 Credential Provider IPC 语义保持不变。
- worker 不读取 `users.dat`，不接触明文密码、DPAPI 密文或完整 `AUTH_SUCCESS`。
- 摄像头只在 `AUTH_START` 后打开；锁屏预加载只加载模型和运行库。
- worker 或资源异常时 fail-closed，只允许用户改用 Windows 密码，不回退到父进程内认证。
- 摄像头不可用后阻断密码输入的问题已定位并修复。

## 2. 最终运行架构

```text
LogonUI.exe
  └─ FaceLoginCredentialProvider.dll
       │ 公共命名管道：AUTH_REQUEST / STATUS / AUTH_SUCCESS / 终态
       ▼
FaceLoginService.exe（长期父服务）
  ├─ SCM / WTS 事件
  ├─ Credential Provider 公共命名管道
  ├─ users.dat / CredentialStore / DPAPI
  ├─ SID 匹配、最终单次密码解密
  └─ worker supervisor + Job Object
       │ 两根私有继承管道，仅传配置、状态、embedding 和判定
       ▼
FaceLoginService.exe -auth-worker（一次性 LocalSystem / Session 0）
  ├─ DirectShow
  ├─ SCRFD / R50 / 双 MiniFAS
  ├─ PAD 5/5、bbox 连续性、embedding
  └─ 一次认证结束后退出
```

生命周期：

1. `SESSION_LOCK` / `LOGOFF`：父服务启动 worker；worker 校验四个模型并发送 `READY`，但不打开摄像头。
2. `AUTH_REQUEST`：若 worker 已就绪则立即开始，否则 Credential Provider 显示“正在加载模型”。
3. worker 收到 `AUTH_START` 后才打开摄像头，并执行 PAD 与身份绑定。
4. 第 1/3/5 个有效 PAD 帧的 512 维 embedding 发给父服务；父服务只返回通过、重试或拒绝。
5. 三次匹配同一 SID 且 PAD 5/5 全部通过后，父服务只解密一次密码并发送公共 `AUTH_SUCCESS`。
6. worker 在一次认证终态后退出。认证失败且桌面仍锁定时，父服务后台补载新 worker。
7. `SESSION_UNLOCK` / `LOGON` 或服务停止时关闭 worker Job；解锁后的正常状态为 1 个父服务、0 个 worker。

## 3. 实现完成情况

| 原方案项目 | 完成状态 | 当前实现与证据 |
|---|---|---|
| 父服务资源边界 | 已完成 | 生产 `FaceService` 只管理公开 IPC、凭据、SID 匹配和 worker 生命周期；相机与 ONNX 仅存在于 worker。standalone 使用同一 DirectShow 路径。 |
| 私有 worker 入口 | 已完成 | `main.cpp` 在服务单实例 mutex 前识别私有 `-auth-worker`；worker 验证 LocalSystem、继承句柄与握手，无有效父通道立即退出。 |
| 安全进程创建 | 已完成 | 使用绝对 EXE 路径、完整引号、`CREATE_SUSPENDED`、`CREATE_NO_WINDOW`、`EXTENDED_STARTUPINFO_PRESENT`、`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`；先加入带 `KILL_ON_JOB_CLOSE` 的 Job，再恢复线程。 |
| 私有协议 | 已完成 | 20 字节固定头、16 KiB payload 上限、类型/version/magic/request ID 检查；embedding 必须恰好 512 维、全部有限且 L2 范数合理；binding 只能 1→2→3。 |
| `AuthPipeline` 抽取 | 已完成 | PAD、检测、bbox 连续性、embedding 与三次 binding 统一在 `auth_pipeline.*`；通过回调隔离 PipeServer、CredentialStore、DPAPI 和 FaceService。 |
| P-core 亲和性 | 已完成 | 性能核约束移入 worker，并覆盖 `AUTH_START` 到认证终态窗口。 |
| 凭据安全拆分 | 已完成 | `FindBestIdentity` 只返回身份，不解密密码；`LoadCredentialForSid` 仅在三次同 SID 与 PAD 完成后调用一次；密码和公共消息缓冲立即清零。 |
| fail-closed | 已完成 | 空 SID、记录消失、数据库损坏、模型错误、协议错误、worker 崩溃/超时都不会构造部分凭据，也不会回退到父进程认证。 |
| 单一相机实现 | 已完成 | 运行期、Enrollment、standalone 和工具统一使用 DirectShow；没有后端配置、自动选择或同次认证 fallback。 |
| 一次性 worker | 已完成 | 每个 worker 最多处理一次认证；成功、失败、取消、超时或私有管道断开后均回收。 |
| Credential Provider 失败交互 | 已完成 | 失败终态只原位显示，不调用会触发 LogonUI 重枚举的被动重试；只有用户点击“重新尝试人脸识别”才发起新请求。（2026-08-29 更新：失败 tile 起重新武装 qualifying 输入监听，任意按键/点击亦触发重试，密码输入靠结构隔离排除，见 `docs/modules/windows-clients.md`。） |
| 构建、打包、文档 | 已完成 | Release、Vue、Go、Wails 构建通过；私有协议、模块、运维、构建和性能文档均已同步。 |

主要新增实现：

- `face_service/auth_pipeline.*`
- `face_service/auth_worker.*`
- `face_service/auth_worker_client.*`
- `face_service/auth_worker_protocol.*`
- `face_service/performance_affinity.*`
- `credential_provider/auth_interaction_policy.h`
- `common/model_hashes.h`

主要新增测试：

- `AuthWorkerProtocolTest`
- `AuthWorkerLifecycleTest`
- `CredentialStoreTest`
- `CredentialProviderInteractionPolicyTest`
- `IpcProtocolTest`
- `ModelIntegrityTest`
- 扩展后的 `CameraLifecycleTest`

## 4. 性能结论

### 4.1 738.4 ms 与 1285.8 ms 的口径

早期看到的两个数字不能直接相减：

- 旧同进程 DS 的 **738.4 ms** 是 `camera ready → Credentials`，不包含 DirectShow 建图。
- 早期 worker DS 的 **1285.8 ms** 是 `AUTH_START → Credentials`，包含相机初始化。
- 把 worker 起点对齐到相机 ready 后，早期 worker 核心为 **796.1 ms**，相对旧路径增加 57.7 ms，而不是表面上的 547.4 ms。

成功路径日志优化后，正式 worker + DS 通过性能门槛：

| 指标 | 旧同进程 DS | worker + DS | 变化 / 结论 |
|---|---:|---:|---|
| `camera ready → Credentials` 平均 | 738.4 ms | **769.3 ms** | +30.9 ms，低于 +50 ms 门槛 |
| 核心 p95 | 789 ms | **844.0 ms** | 低于旧值 110% 上限 867.9 ms |
| CP 请求 → 解锁平均 | 1483 ms | **1532.1 ms** | +49.1 ms，低于 +50 ms 门槛 |
| CP 请求 → 解锁 p95 | 参考上限 1732.5 ms | **1668 ms** | 通过 |
| 正式样本成功率 | — | **30/30** | 通过 |

### 4.2 MF Session 0 A/B

MF 完成 32/32 次认证，正式统计取前 30 次。它的相机初始化比 worker DS 平均快 234.9 ms，但首个有效帧和完整 pipeline 更慢：

| 指标 | worker + DS | worker + MF | MF 相对 DS |
|---|---:|---:|---:|
| `AUTH_REQUEST → Credentials` 平均 | 1194.7 ms | 1282.3 ms | +87.6 ms |
| `camera ready → Credentials` 平均 | 769.3 ms | 1091.7 ms | +322.4 ms |
| CP 请求 → 解锁平均 | 1532.1 ms | 1637.1 ms | +105.0 ms |

该实验路径未达到“平均至少改善 50 ms、p95 不恶化、成功率相同”的门槛，现已从产品删除。

## 5. 长期资源验收

最终安装服务使用 DirectShow 连续完成 **105/105** 次锁屏人脸登录。按正式口径跳过首次预热，以连续 100 个完成周期统计：

| 父服务资源 | 首值 | 尾值 | 增量 | 验收结果 |
|---|---:|---:|---:|---|
| handles | 274 | 274 | **0** | ≤ 2，通过 |
| private bytes | 3.8 MiB | 4.5 MiB | **+0.7 MiB** | ≤ 10 MiB，通过 |
| working set | 17.2 MiB | 18.0 MiB | +0.8 MiB | 无线性退化 |
| threads | 6 | 5 | **−1** | 无线性增长 |

105 个 worker PID 全部不同且全部退出；解锁后 worker 数量为 0，无孤儿进程。DirectShow 在单个 worker 内仍可能留下驱动 `EtwRegistration`，但不会越过进程退出边界传给父服务。

重新构建后的 `AuthWorkerLifecycleTest --cycles 100` 也保持 136 handles、4 threads、2.54 MiB private，逐轮无子进程残留。

## 6. 实机与故障验证

已完成：

- MSA 正常锁屏人脸登录与解锁。
- 冷启动重启后的首次登录；模型预加载不打开摄像头，首次请求后才建图。
- 普通锁屏、注销后登录、失败后显式重试。
- 无人脸 2.5 秒快速失败；无部分凭据，失败 worker 退出并后台补载。
- 手机/屏幕照片攻击被 PAD 拒绝；PAD 5/5 规则未放宽。
- S3 睡眠恢复后立即认证。
- FaceLoginConsole 正在使用摄像头时锁屏，Console 释放后 worker 接管；解锁后 Console 恢复预览。
- 摄像头全局隐私权限关闭：明确报错、允许密码登录、无自动重试风暴。
- PnP 禁用摄像头模拟设备拔出：只返回摄像头不可用，无 `AUTH_SUCCESS`；恢复设备后可重新取帧。
- 公共 Credential Provider 管道在认证中途断开：正在使用的 worker 被取消并退出，无相机或凭据残留。
- worker 启动、模型加载、相机、PAD、第三次 binding 崩溃；超时、断管、错误 request ID、错误 binding 顺序和无效 embedding。
- 实际安装目录模型翻转 1 字节：在创建 ONNX Session 前因 SHA 不匹配 fail-closed；恢复模型后自动恢复。
- 父进程退出时 Job 自动清理子进程，无孤儿 `FaceLoginService.exe -auth-worker`。

### 摄像头故障阻断密码输入修复

旧 Credential Provider 在相机失败后触发 `CredentialsChanged`，LogonUI 的重新枚举会重新启动全局输入检测线程，导致密码按键被误判为人脸认证触发，表现为密码输入偶发被打断。

修复后：

- 超时、相机不可用、PAD 拒绝和服务错误只在当前磁贴原位显示。
- 失败状态不重新枚举、不重新监听键鼠。
- 密码输入期间不会自动发起新的 `AUTH_REQUEST`。
- 只有点击“重新尝试人脸识别”才允许新请求。

隐私权限关闭的实测中只产生 1 次请求和 1 个失败 worker；随后约 9 秒密码输入期间无第二次请求，并成功使用系统密码解锁。

## 7. 自动化与构建结果

最终执行并通过：

```powershell
$CMAKE --build build --config Release
.\build\tools\auth_worker_protocol\Release\AuthWorkerProtocolTest.exe
.\build\tools\credential_store\Release\CredentialStoreTest.exe
.\build\tools\credential_provider_policy\Release\CredentialProviderInteractionPolicyTest.exe
.\build\tools\model_integrity\Release\ModelIntegrityTest.exe
.\build\tools\ipc_protocol\Release\IpcProtocolTest.exe
.\build\tools\auth_worker_lifecycle\Release\AuthWorkerLifecycleTest.exe --cycles 100
```

相机隔离测试已完成：

```powershell
.\build\tools\camera_lifecycle\Release\CameraLifecycleTest.exe --mode child --cycles 100 --settle-ms 500
```

DirectShow 完成 100/100，每轮至少取得 10 个 640×480 有效帧，父进程资源无随轮次线性增长。

安装器侧：

```powershell
cd installer\FaceLoginSetup\frontend
npm run build

cd ..
go test -count=1 ./...
wails build -clean -platform windows/amd64
```

全部通过。安装器新建配置的默认值也有 Go 测试固定：

- `match_threshold: 0.80`
- `anti_spoof_threshold: 0.281`

## 8. 最终部署产物

最终安装器：

```text
installer/FaceLoginSetup/build/bin/FaceLoginSetup.exe
大小：95,482,368 bytes
SHA-256：C70586693D0AA49FC85D0FFB1E27C693D1D6FAE176B7FC083FDE8F63215172C6
```

内嵌并实测的核心文件：

| 文件 | SHA-256 |
|---|---|
| `FaceLoginService.exe` | `D382BC92A5D032E527308D621257C432A8E2B97A29BE3F80A0ED21076CA404B6` |
| `FaceLoginCredentialProvider.dll` | `C7833F76FCACFC01B2B3A2C1530BE3AB81BD9B9506D6E4EA97839611E51C386A` |
| `FaceLoginConsole.exe` | `F2C49780120CA6C4149DE98BBAAF4A6C449852B6AA161923D8E40402B6D434E3` |

安装资源白名单已核对为：3 个产品二进制、5 个运行时 DLL、4 个正式 ONNX 模型，无临时文件或备份残留。资源镜像和当前 `C:\Program Files\FaceLogin` 中对应文件逐项 SHA-256 一致。

最终运行状态：服务 `Running / Auto`；已解锁桌面为 1 个父服务、0 个 worker；摄像头 `Integrated Camera: OK`；实际配置为 `match_threshold: 0.8`。

## 9. 用户明确免测项

以下项目没有执行完整实机测试，并且**不记为通过**：

| 项目 | 记录 |
|---|---|
| 普通本地 Windows 账户完整登录 | 用户明确免测；没有创建账户，也没有使用系统测试账户。公共 IPC 本地账户格式已有自动化覆盖。 |
| 未录入人脸的另一位真人／真人换脸 | 用户明确免测；照片 PAD、三次同 SID、错误 SID 与 binding 协议拒绝不能冒充该实机项目通过。 |

## 10. 方案边界与后续事项

本次目标已经完成，但以下事项没有被错误地并入本次结论：

- 不把 worker 放进交互用户 Session；生产边界仍是 LocalSystem / Session 0。
- 不恢复相机后端选择或同次认证 fallback；产品只保留 DirectShow。
- 不改变 PAD 5/5、三次身份绑定、匹配阈值、安全超时或公共 IPC 语义。
- 未提交的通用加固与工程事项继续以 [`todo.md`](todo.md) 为准，例如日志 ACL、模型 hash 跨语言单一来源、face align/config 边界测试和 trace ID；它们不是本次 worker 迁移与资源验收的未完成项。

## 11. 最终判定

一次性 worker 已解决原长期父服务中的 DirectShow 驱动句柄累积和 ORT/Windows 堆高水位问题，并在保持认证安全语义的前提下通过性能门槛、100 周期资源门槛、安装服务实测、故障注入、构建与打包验收。

生产决策固定为：**一次性 worker + DirectShow**。
