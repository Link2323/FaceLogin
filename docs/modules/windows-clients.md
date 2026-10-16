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
- 单次认证从进入 `Authenticating` 起最多等待 5 秒，连接、请求写入和后台读取共用同一单调时钟截止时间。后台读即使只收到 `STATUS:` 或完全无响应也主动超时并关闭管道，界面显示“识别超时”；不依赖 `GetSerialization` 轮询。迟到终态不能放行凭据，重试另起一轮预算。

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
- `scripts/unregister_dev.bat`（开发环境卸载脚本）

修改必须全部同步（前四处由安装器的 `internal/cross_language_consistency_test.go` 校验）。DLL 使用静态 CRT `/MT`，不依赖安装包的五个 ONNX Runtime DLL。

## 注册控制台

目录：`enrollment_app/`

`FaceLoginConsole.exe` 是管理员运行的 Win32 + WebView2 应用。`EnrollmentWizard` 负责预览、采样和录入流程，协调采样、配置与账户模块调用；`WebviewHost` 把后端通过 `IDispatch` 暴露给嵌入的 `index.html`。

注册端的 Windows 会话查询与密码验证集中在 `session_identity.*`；`service_client.*` 管理数据库重载、配置重载和学习状态查询的管道连接、等待及 ACK；`log_viewer.*` 管理四个固定日志来源的读取、旧 UTF-16 兼容和清空。这三者组成 `facelogin_enrollment_support` 静态库，供 Console 与独立测试使用，不依赖 WebView2、相机或推理模型。

预览与注册使用 [`vision/`](../../vision/) 的相机和推理库，凭据保存与人脸管理使用 [`storage/`](../../storage/) 的数据库库。注册程序通过链接目标复用它们，不再包含或编译 `face_service/` 的内部文件。

控制台 UI 线程由 WebView2 初始化为 STA。DirectShow 预览必须复用该 COM 公寓，不能再次请求 MTA，否则会得到 `RPC_E_CHANGED_MODE` 并使预览在模型初始化前失败。服务 worker 则在其自己的 MTA 中运行。 预览与解锁共用 `WebcamCaptureDS`（face exposure tune 在其调用链上），两侧曝光行为一致；预览分辨率 1280×720、worker 640×480，驱动 AE 为传感器级测光不受分辨率影响。

`preview_exposure.*` 的 `PreviewExposure` 管理预览曝光会话：启动回放、初次调节、后台 watch、冷却计时及待确认恢复状态。`EnrollmentWizard` 保留相机、模型和预览线程所有权，在线程启动前准备曝光会话，帧线程逐帧调用观察入口，停止并 join 后释放会话。回放策略与认证共用 `ReplayExposureState`，模型加载与认证预热各自吸收回放等待时间。预览启动调节和后台曝光 watch 共用一份会话内待确认恢复状态。调节写入的稳定测量中断后，watch 每 3 秒遇到落带人脸时可用共享稳定门接续复核，不受写参数的 10 秒冷却限制；仅参数与手动模式在测量前后一致、严格递增的新帧已稳定且仍落带时保存 `camera_tune.state`。复核不写硬件，采集时跳过，停止或采集中止确认，重新建图清除旧状态；帧缓存仍由原 mutex 保护。身份/PAD 结果不能代替曝光稳定证据。

`preview_frame_encoder.*` 独立管理 WIC 工厂与逐帧 JPEG/Base64 编码，质量维持 0.70。工厂在 UI 的既有 COM 公寓中创建，预览线程串行编码，join 后销毁；模块不初始化或反初始化 COM。编码各阶段失败时返回空输出，临时 COM 对象自动释放。

### 账号身份

`enrollment_accounts.*` 的 `EnrollmentAccounts` 拥有注册端凭据库，集中处理模板保存、人脸管理及账户身份修复；`EnrollmentWizard` 不再直接读写数据库。所有写入经 `SaveAndReload` 先保存，再等待服务确认。录入保存结果单独报告是否已经落盘：落盘后即消费采样证明，即使服务确认失败也不重复使用样本。数据库解析失败时终止本次写入。账户变化提示的状态、脸数和当前 UPN 来自同一次查询快照。

`session_identity.*` 在构造时读取身份快照，账号变化检测与密码验证时重新查询当前会话。身份来源为：

1. `GetUserNameW` 获取 SAM 用户名。
2. `GetUserNameExW(NameUserPrincipal)` 获取 UPN。
3. UPN 不可用时，仅从当前进程令牌的已启用 MSA 身份组解析邮箱，不使用机器级 IdentityStore 缓存。
4. `LookupAccountNameW` 优先按 UPN、再按 SAM 用户名获取身份快照中的 SID；人脸追加授权和账号记录修复另行读取当前进程令牌的用户 SID。

检测 MSA ↔ local 变化时必须保留已有脸并正确更新/清除 UPN。人脸录入要求账户具有 Windows 或 MSA 密码。

### 多角度注册

`enrollment_capture.*` 的 `EnrollmentCapture` 拥有采样线程、角度进度、活体状态和未保存样本。`EnrollmentWizard` 仅提供受 `m_frameCacheMutex` 保护的帧快照，模型保持存活直到采样 `Stop()` 返回。每次采集固定使用启动时的活体阈值，设置更新对下一次采集生效。重复启动在采样尚未退出时直接失败，不阻塞 UI；取消请求与线程实际完成分别记录，只有完成后才能恢复曝光调节或导出用于保存的样本快照。重新采集某个角度时，一并丢弃该角度和后续角度的旧样本及计数。

保存入口通过 `CopyCompletedSamples` 获取已校验的独立快照，不再直接读取工作线程的样本数组；`enrollment_templates.*` 先检查样本维度、有限值和分组完整性，再逐角度检查一致性、平均并归一化；所有模板准备成功后才进入凭据库更新，失败不会返回部分模板。保存成功后清除会话样本和活体证明。对外 JSON 字段保持不变，`done` 表示线程已退出。

每个正面/左转/右转角度独立执行：

1. SCRFD bbox + 5 点和 yaw 门控。
2. 双 MiniFAS PAD。
3. 收集该角度的 128-D SFace embedding。
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

- `index.html.in`、`styles.css` 与 `enrollment_app/` 下的 JS 模块在 CMake 配置时按 `ENROLLMENT_JS_SOURCES` 列表顺序拼接为单个 `index.html`，编译进 `FaceLoginConsole.exe`；修改任一源文件后必须重建 C++ 目标。
- JS 桥接的 dispId 清单以 `WebviewHost.cpp` 为准，不在文档复制。
- 日志页四个日志来源直接读文件；"学习状态"视图是唯一走公共管道的查询（`LEARNING_STATUS`，响应为单条 JSON 内存快照，服务重启清零），客户端按 10s 节流——公共管道单实例，不得演变成高频轮询。
- 禁用右键菜单和开发者工具。
- `WM_WTSSESSION_CHANGE` 在锁屏时释放摄像头，解锁后仅当锁屏瞬间预览在跑（录入页可见）才恢复——JS 层切到人脸/设置/日志页即关相机，无条件恢复会让相机亮在无预览的页面背后。
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
- 键盘/鼠标触发、取消及密码磁贴切换
- COM 注册/注销
- 无用户时磁贴隐藏
- 登录与解锁场景
- 服务不可用/超时/用户切换
- 密码缓冲区和日志审计

Enrollment 改动：

- 构建 `FaceLoginConsole`；服务控制通信和日志改动还需构建、运行 `EnrollmentSupportTest`（独立测试管道、临时日志目录）及 `IpcProtocolTest`
- 管理员启动与提权
- 预览/锁屏摄像头释放
- 三角度采集与一致性拒绝
- 本地账号、MSA、空密码账户拒绝录入
- 保存、热重载、删除/重命名人脸
- 修改页面模板、样式或脚本后确认实际重建并嵌入新 UI

采集页面由 `capture_flow.js` 管理整轮状态、轮询与角度间延迟。取消、失败和完成统一结束本轮，清理定时器并使旧回调失效；下一轮保留独立代次，避免快速重试时旧回调启动下一角度或覆盖提示。页面其余逻辑按领域拆分为 `enrollment_app/js/` 下的 JS 模块（`ui_common`/`preview`/`capture_ui`/`navigation`/`settings`/`faces`/`account_refresh`/`enroll_save`/`logs`/`app_main`），宿主调用与呈现分散其中；跨模块共享状态收在所属模块闭包内（`previewCtl`、`captureCtl`、`navCtl` 等），HTML 内联 onclick 引用的处理器保持全局函数。
