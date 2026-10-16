# 解锁流程安全与稳定性 TODO

> 本清单来自 2026-08-09 对“锁屏触发 → Credential Provider → 命名管道 → 人脸/PAD/身份匹配 → LSA 凭据提交”链路的静态审查。
>
> 序号按影响从高到低排列，不代表开发成本。排序采用“后果严重度 → 正常路径触发概率 → 现有缓解 → 恢复范围”：凭据泄露优先于进程完整性破坏，进程完整性破坏优先于服务永久卡死，之后是受支持环境无法使用、误拒和资源泄漏。需要远程管理员权限、已有 DACL 缓解或仅在异常销毁窗口触发的问题，会在同等后果下后移。
>
> 本文只跟踪本轮新增发现；已关闭的问题会从本清单删除，不保留历史条目。

优先级定义：

- **P1**：可能泄露 Windows 凭据、破坏 LogonUI 进程完整性、永久占用单实例认证服务，或在受支持 Windows 配置下稳定阻断人脸解锁；发布前必须修复。
- **P2**：安全边界或进程稳定性风险已有管理员权限/DACL/异常触发窗口等缓解，或只造成可恢复的误拒与资源增长；应在下一批稳定性修复中处理。
- **P3**：只影响旧版本混用或低概率兼容路径，当前同版本部署不受影响。

难度标记与 P1/P2/P3 独立：**C** 为局部小改动或脚本，**B** 为单模块改造并补测试，**A** 为跨模块/跨线程协调，**S** 为 Windows 多线程 COM 或管道 I/O 生命周期重构。

任务边界：

- TODO 2 拥有 Credential Provider 的认证状态、管道回调和 COM 事件接口生命周期；TODO 5 只拥有输入检测线程及可取消连接。两项共同接触 `StartAuth` / `UnAdvise` 时以 TODO 2 的状态所有权为准。

## 修复顺序

### 1. [ ] P1 / A — 收敛并擦除 CP 自有的明文密码副本

**影响**：`m_password` 虽然会清零，但 `PipeClient::m_readBuffer`、读取线程局部 `msg`、`CheckResponse` 的 `response`，以及 `ParseAuthMessage` 的 `payload` / `passwordPart` 都保存过完整密码。普通 `std::wstring` 析构不会擦除缓冲区，密码可能在 LogonUI 堆中残留到内存被复用或进程退出。本项只约束 FaceLogin CP 自己拥有的缓冲区；已经通过 `rgbSerialization` 移交给 LogonUI/LSA 的凭据缓冲区遵守 Windows 所有权契约，不由 CP 提前擦除。

**位置**：

- `credential_provider/pipe_client.cpp:176-243, 275-324`
- `common/ipc_protocol.cpp:18-81`
- `credential_provider/FaceLoginCredential.cpp:584-650, 939-997`

**修复方向**：

- 为承载 `AUTH_SUCCESS` 的传输缓冲区和临时字符串建立统一的敏感数据所有者与 RAII 擦除策略。
- `CheckResponse` 复制出消息后立即清零 `m_readBuffer`；`Disconnect` / 析构也必须兜底清零。
- 避免在解析过程中构造多个密码 `wstring` 副本；必要时改为就地解析或显式可擦除缓冲区。
- 检查所有失败、超时、断开和对象析构路径，不能只覆盖成功提交路径。

**验收**：

1. 在 `PackCredentials` 返回、`Disconnect` 返回和 CP 析构三个观测点，分别覆盖成功、服务断开和打包失败，调试器扫描 `PipeClient` 缓冲区与 FaceLogin 自有临时分配，不再找到测试密码；不扫描已移交给 LogonUI/LSA 的 `rgbSerialization`。
2. 日志中没有 `AUTH_SUCCESS` 载荷或密码片段。
3. 含冒号和非 ASCII 字符的密码仍能正常解锁。

### 2. [ ] P1 / S — 关闭 Credential Provider 的跨线程数据竞争与释放竞态

**影响**：管道线程会修改 `m_state`、`m_statusText`、账户字段和密码，并调用 `m_pCredentialEvents` / `m_pProviderEvents`；COM 主线程同时读取这些字段或在 `UnAdvise` 中释放接口。现有 `m_cs` 没有覆盖完整状态，可能造成 `std::wstring` 堆破坏、已释放 COM 指针调用或 LogonUI 崩溃。LogonUI 崩溃的影响超出 FaceLogin 磁贴本身。

**位置**：

- `credential_provider/FaceLoginCredential.cpp:239-317`
- `credential_provider/FaceLoginCredential.cpp:393-446, 514-690`
- `credential_provider/FaceLoginCredential.cpp:927-1007`
- `credential_provider/FaceLoginCredential.h:109-143`

**修复方向**：

- 采用“不可变快照 + 受锁交换”模型：后台线程构造完整 `AuthResult` / status 快照，只在持有 `m_cs` 时一次性交换；所有 COM getter 在同一把锁下复制快照后再使用，禁止对 `std::wstring` 做无锁读写。
- `Advise` 在 COM 线程把 `ICredentialProviderCredentialEvents` / `ICredentialProviderEvents` 注册到 Global Interface Table（GIT）；后台线程初始化 COM，并只通过 GIT 取得的代理发通知，禁止跨线程读取或调用原始事件接口指针。
- 后台线程在创建前对 Credential 持有显式引用，退出时释放；禁止捕获无生命周期保证的裸 `this`。
- `UnAdvise` 的固定顺序为：禁止新通知 → 发停止信号 → 等待后台线程退出 → 撤销 GIT cookie → 清除受锁状态。释放事件接口前不能仍有可执行回调。
- 不在持锁状态下调用外部 COM 接口，避免重入死锁。

**验收**：

1. 增加仅调试构建启用的 barrier：分别暂停在“终态快照交换前/后”和“`UnAdvise` 禁止回调前/后”，枚举四种交错各运行 1,000 次，LogonUI 不崩溃、不死锁；调试断言确认后台通知使用 GIT 代理，未直接解引用原始事件接口指针。
2. Application Verifier / PageHeap 下连续 100 次锁屏认证无 UAF、堆损坏或锁错误。
3. TSAN 不适用于该 Windows/COM 路径时，至少增加可重复的状态机单元测试和人工竞态注入。

### 4. [ ] P1 / A — 按 usage scenario 生成正确的 Windows 凭据结构

**影响**：Provider 接受 `CPUS_UNLOCK_WORKSTATION`，但 `PackCredentials` 永远以 flags=0 调用 `CredPackAuthenticationBufferW`，生成 `KERB_INTERACTIVE_LOGON`，没有为 unlock scenario 生成 `KERB_INTERACTIVE_UNLOCK_LOGON`。Windows 10/11 在部分组策略、控制台/远程会话和快速用户切换配置下仍可能请求该场景，届时 FaceLogin 可能稳定失败并只能回退密码登录。

**位置**：

- `credential_provider/FaceLoginProvider.cpp:134-243`
- `credential_provider/FaceLoginCredential.cpp:811-906`

**修复方向**：

- 将 `CREDENTIAL_PROVIDER_USAGE_SCENARIO` 传入凭据打包逻辑。
- `CPUS_LOGON` 使用 `KerbInteractiveLogon`；`CPUS_UNLOCK_WORKSTATION` 使用 `KerbWorkstationUnlockLogon`，按微软示例正确打包 `KERB_INTERACTIVE_UNLOCK_LOGON`。
- 保留当前 Windows 10/11 常见的 `CPUS_LOGON` 解锁路径，同时覆盖策略强制的 unlock scenario。

**验收**：

1. 增加纯打包测试，分别传入 `CPUS_LOGON` 和 `CPUS_UNLOCK_WORKSTATION`，解包后断言 `MessageType` 分别为 `KerbInteractiveLogon` 和 `KerbWorkstationUnlockLogon`。
2. 安装环境至少完成：常规锁屏的 `CPUS_LOGON`、一组日志确认 `SetUsageScenario cpus=CPUS_UNLOCK_WORKSTATION` 的策略/会话配置；两组都覆盖本地账户和 MSA。
3. `ReportResult` 能正确处理两种场景的成功与密码失效。

### 5. [ ] P2 / A — 移除 `TerminateThread`，修正输入检测线程所有权

**影响**：输入线程在 `StartAuth` 中可能阻塞最多 5 秒，`StopInputDetectionThread` 只等待 2 秒便调用 `TerminateThread`。若线程当时持有 CRT、日志或内存分配内部锁，可能破坏 LogonUI 进程状态。线程自然退出时先把 `m_inputThreadRunning=false`，停止函数会提前返回，线程句柄和停止事件无法关闭；失败重试会持续泄漏句柄。

**位置**：

- `credential_provider/FaceLoginCredential.cpp:59-149, 697-805`
- `credential_provider/pipe_client.cpp:45-98`
- `credential_provider/pipe_client.h:33-56`

**排序说明**：潜在后果可影响 LogonUI，但只在输入线程正阻塞连接且对象于 2 秒窗口内销毁时触发，正常认证路径不会调用 `TerminateThread`，因此排为 P2。

**修复方向**：

- `PipeClient::Connect` 接受停止事件并使用可取消等待；禁止使用 `TerminateThread`。
- 线程生命周期以 `m_hInputThread` 是否存在为准，运行标志改为原子状态或由单线程维护。
- 无论线程自然结束、创建失败还是主动停止，都统一 join/close 句柄和事件。

**验收**：

1. 服务不可用时启动连接，在 100 毫秒、1 秒和 4 秒时切换磁贴或销毁 Provider；输入线程均在停止信号后 1 秒内正常退出，不调用 `TerminateThread`。
2. 连续 100 次失败重试；输入线程句柄和停止事件增加仅测试构建可见的 ownership 计数器，每轮结束都必须回到 0，且第 100 轮的 `GetProcessHandleCount` 不得高于首轮结束值 2 个以上。
3. 用线程 ID 和调试生命周期计数器确认 Provider 析构返回时输入线程已退出；管道线程生命周期由 TODO 2 验收。

## 总体验收门槛

完成以上任务后，至少执行：

1. Release 构建 `FaceLoginService` 与 `FaceLoginCredentialProvider`。
2. standalone 验证正常成功、无人脸、PAD 拒绝、模型失败、客户端断开和超时。
3. 安装环境验证本地账户与 MSA 的锁屏解锁，以及密码失效后的 `ReportResult`。
4. 故障注入：客户端不发请求、不读响应、读线程创建失败、终态到达时切换磁贴、服务停止。
5. 连续 100 次锁屏循环，同时记录 LogonUI/服务的句柄、private bytes、崩溃和认证结果。
6. 审计 `service.log` 与 `credential_provider.log`，确认没有明文密码或 `AUTH_SUCCESS` 载荷。
