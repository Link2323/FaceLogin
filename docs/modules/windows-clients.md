# Windows 客户端组件

本文覆盖锁屏 Credential Provider 和注册控制台。两者都调用 `facelogin_common`，但运行环境、权限和 DataPath 处理不同。

## Credential Provider

目录：`credential_provider/`

`FaceLoginCredentialProvider.dll` 被 `LogonUI.exe` 以 COM Apartment 模型加载，实现 `ICredentialProvider` 和 `ICredentialProviderCredential`。它不是独立进程。

### 显示与认证

- 支持 `CPUS_LOGON` 和 `CPUS_UNLOCK_WORKSTATION`。
- `FaceLoginProvider::ReadUserCountFromDatabase` 只读 `users.dat` header；无注册账号时隐藏磁贴。
- 登录与解锁行为一致：输入检测线程只在 `SetSelected()`（用户真正落到本磁贴）启动；`Advise()` 不监听全局输入，避免从未选中本磁贴的 MSA PIN 重置等流程中打字误开摄像头。`SetDeselected()` 先停止监听，再断开进行中的认证管道并释放摄像头；迟到响应由 `ShouldProcessPipeResponse` 丢弃。
- 输入触发采用 `input_trigger_policy.h` 的**边沿状态机**，不再读取 `GetLastInputInfo`，也没有静默窗、沉降窗或键盘重复速度假设。Raw Input 键盘 MAKE/BREAK 进入纯策略类；鼠标只轮询物理按钮状态，移动事件没有进入策略的入口，因此永不触发。输入线程以 10ms 粒度采样按钮并通过 `MsgWaitForMultipleObjectsEx` 泵送 `WM_INPUT`。
- 保留 Windows 原生锁屏壁纸时的产品语义：普通键盘键按一次即触发（壁纸阶段 MAKE 可能被吞，普通孤立 BREAK 仍算一次完整按压）；鼠标第一次点击只推走壁纸、第二次在凭据视图完成 DOWN→UP 后形成触发候选。鼠标候选有 100ms 可取消窗口：若该点击切换到密码/PIN 磁贴，`SetDeselected()` 会先停止监听，摄像头不启动；只有仍停留在本磁贴时才开始认证。单纯移动鼠标只推走壁纸，不启动摄像头。Raw Input 注册失败时降级为凭据视图阶段的 `GetAsyncKeyState` 键盘边沿轮询。
- Win+L 残留按**事件归属**排水：初始锁屏轮吞掉第一条 L 事件链（含任意数量自动重复直到 BREAK），没有本轮 MAKE 的 LWIN/RWIN BREAK 永不构成按压。它覆盖 Win+L 一起释放、先松 Win 后松 L、先松 L 后松 Win三种长按顺序，且不依赖重复延迟/频率。代价是无法从 CP 内区分“Win+L 的 L 尾巴”和“其它方式锁屏后立即第一次按 L”，后者保守地需要再按一次；这是保留壁纸且不增加锁前常驻输入代理时的明确边界。完整排水后下一次新按键立即触发。
- 失败磁贴使用独立的 `FailureRetry` 轮次：启动时快照当前仍按住的键和鼠标按钮，自动重复与释放尾巴都不触发，只有快照之后新的键盘 MAKE 或完整鼠标点击才调用 `StartExplicitRetry`。密码输入靠结构隔离：本磁贴无编辑字段，切换磁贴先停止监听；密码命令链接入口也主动停止 watcher，并断开可能已抢跑的认证管道。服务不可用导致监视线程内立即失败时仍不自动重武装；重新选中磁贴后再武装。失败磁贴无命令链接，字段 4 显示“请按任意键重试”；原因文案与密码/重录指引规则保持不变。
- `STATUS:` 更新 UI；终态到达后先发送 `AUTH_ACK`，再进入凭据状态回调。
- 管道读写使用 overlapped 完成事件；`SetDeselected()` / `UnAdvise()` 通过停止事件和 `CancelIoEx` 立即取消，并在释放缓冲区前回收 I/O。管道读取不得阻塞 LogonUI 主线程。

### 凭据打包

认证成功后：

1. 解析 SID、UPN、域/用户名和密码。
2. 通过 `LsaConnectUntrusted` / `LsaLookupAuthenticationPackage` 获取认证包。
3. `CredPackAuthenticationBufferW` 打包交互式登录凭据。
4. 本地账号使用 `Domain\Username`；MSA 优先使用 UPN。
5. 返回 `KerbInteractiveLogon` 序列化结果。
6. 所有密码字符串和消息缓冲区立即清零。

### COM 与注册

CLSID：`{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}`。

字面量同时存在于：

- `credential_provider/resource.h`
- `credential_provider/dllmain.cpp`
- `credential_provider/FaceLoginProvider.cpp`
- `installer/FaceLoginSetup/internal/com.go`

修改必须四处同步。DLL 使用静态 CRT `/MT`，不依赖安装包的五个 ONNX Runtime DLL。

## 注册控制台

目录：`enrollment_app/`

`FaceLoginConsole.exe` 是管理员运行的 Win32 + WebView2 应用。`EnrollmentWizard` 负责摄像头、检测/PAD、身份、密码验证和持久化；`WebviewHost` 把后端通过 `IDispatch` 暴露给嵌入的 `index.html`。

控制台 UI 线程由 WebView2 初始化为 STA。DirectShow 预览必须复用该 COM 公寓，不能再次请求 MTA，否则会得到 `RPC_E_CHANGED_MODE` 并使预览在模型初始化前失败。服务 worker 则在其自己的 MTA 中运行。

### 账号身份

构造/刷新身份时依次使用：

1. `GetUserNameW` 获取 SAM 用户名。
2. `GetUserNameExW(NameUserPrincipal)` 获取 UPN。
3. `GetUserNameExW(NameSamCompatible)` 获取 `DOMAIN\User`。
4. `LookupAccountNameW` 获取 SID。
5. IdentityStore 注册表缓存作为 MSA UPN 回退。

检测 MSA ↔ local 变化时必须保留已有脸并正确更新/清除 UPN。人脸录入要求账户具有 Windows 或 MSA 密码。

### 多角度注册

每个正面/左转/右转角度独立执行：

1. SCRFD bbox + 5 点和 yaw 门控。
2. 双 MiniFAS PAD。
3. 收集该角度的 512-D embedding。
4. 做角度内一致性检查。
5. 独立 `AddFace` 保存一条记录。

跨角度禁止平均 embedding。三个角度正好占满当前 `kMaxFacesPerUser = 3`。

### 密码与保存

- `LogonUserW` 验证本地账号或 MSA UPN 密码。
- 空密码账户不支持录入或人脸解锁。
- 密码经 DPAPI machine scope 加密后写入 `users.dat`。
- 保存数据库后通过 `RELOAD_DB` 通知服务，并等待 `RELOAD_OK` 后才向 UI 报告完成；管道忙时会有界重试。读到响应后发送 `CONTROL_ACK`，使服务无需阻塞式 flush 即可安全断开。禁止 fire-and-forget。
- 配置保存后通过 `CONFIG_RELOAD`，失败时认证保持 fail-closed。
- 服务日志直接读取 `<DataPath>\log\service.log`，避免大 JSON 经过固定管道缓冲区。

### WebView2 边界

- `index.html` 编译进 `FaceLoginConsole.exe`，修改后必须重建 C++ 目标。
- JS 桥接的 dispId 清单以 `WebviewHost.cpp` 为准，不在文档复制。
- 日志页四个日志来源直接读文件；"学习状态"视图是唯一走公共管道的查询（`LEARNING_STATUS`，响应为单条 JSON 内存快照，服务重启清零），客户端按 10s 节流——公共管道单实例，不得演变成高频轮询。
- 禁用右键菜单和开发者工具。
- `WM_WTSSESSION_CHANGE` 在锁屏时释放摄像头，解锁后恢复。
- 帧缓存由 `EnrollmentWizard::m_frameCacheMutex` 保护。
- 用户数据目录固定为 `%LOCALAPPDATA%\FaceLogin\WebView2`（`WebviewHost.cpp` WM_CREATE 显式传入并递归创建；实际值与创建失败 HRESULT 均写 console.log(原名 enrollment.log)）。创建环境前会删除本进程的 `WEBVIEW2_USER_DATA_FOLDER` 环境变量：官方 loader 对该变量只检查存在性、不检查空值，宿主（如 Wails 安装器经 go-webview2 `preventEnvAndRegistryOverrides` 置空）遗留的空值会静默覆盖代码传参、回退到 exe 旁默认路径——在 Program Files 的 ACL 下 browser 子进程写不进去，controller 创建 19 秒后 E_ABORT。

## DataPath 差异

Credential Provider 和 Enrollment 直接信任注册表 DataPath，读取不到时回退 ProgramData；只有服务使用 `ResolveSecureDataDir` 白名单。因此修改路径逻辑必须同时检查：

- `credential_provider/dllmain.cpp`
- `credential_provider/FaceLoginProvider.cpp`
- `enrollment_app/main.cpp`
- `enrollment_app/EnrollmentWizard.cpp`
- `common/data_path.*`
- `face_service/FaceService.cpp`

不要因为默认安装结果一致，就假设三组件共享同一解析实现。

## 验证

Credential Provider 改动：

- Release 构建
- `CredentialProviderInteractionPolicyTest`
- COM 注册/注销
- 无用户时磁贴隐藏
- 登录与解锁场景
- 服务不可用/超时/用户切换
- 密码缓冲区和日志审计

Enrollment 改动：

- 管理员启动与提权
- 预览/锁屏摄像头释放
- 三角度采集与一致性拒绝
- 本地账号、MSA、空密码账户拒绝录入
- 保存、热重载、删除/重命名人脸
- 修改 `index.html` 后确认实际重建并嵌入新 UI
