# 人脸认证服务

本文解释 `face_service/` 的跨文件行为和修改边界。实现入口是 [`face_service/FaceService.cpp`](../../face_service/FaceService.cpp)；Credential Provider 公共协议、worker 私有协议与持久化分别见 [`IPC 契约`](../contracts/ipc.md)、[`auth-worker IPC 契约`](../contracts/auth-worker-ipc.md) 和 [`users.dat 契约`](../contracts/users-dat.md)。

## 责任边界

`FaceLoginService.exe` 是自动启动的 Windows 服务，也支持 standalone 开发模式。它负责：

- 安全解析 DataPath
- 父进程加载配置、凭据数据库并维持公开命名管道
- 锁屏时预加载一个私有认证子进程；子进程独占四个 ONNX 模型和摄像头
- 检测、PAD、身份一致性和匹配
- 通过事件驱动命名管道向 Credential Provider 返回终态，并以 2 秒有界 `AUTH_ACK` 确认交付

服务不负责注册人脸、验证用户输入的 Windows 密码或安装 COM DLL。

## 生命周期

```text
ServiceMain / RunStandalone
  └─ Initialize
      ├─ ResolveSecureDataDir（拒绝不可信路径）
      ├─ LoadConfig
      ├─ CredentialStore::LoadDatabase
      ├─ 创建 PipeServer
      ├─ 启动模型生命周期线程
      ├─ 登录界面/锁定会话：异步启动并预加载 `-auth-worker`
      └─ 已解锁桌面：保持模型未加载
  └─ Run
      ├─ SESSION_LOCK / LOGOFF → 异步启动一个已加载模型的 worker
      ├─ AUTH_REQUEST → worker 打开相机、运行一次完整管线并退出
      ├─ SESSION_UNLOCK / LOGON → 关闭/终止未使用 worker
      └─ WaitForClient → ReadMessage → 分派请求 → Disconnect
```

父服务不在 `Initialize` 中打开摄像头，也不持有 ONNX Session。worker 在锁屏时只加载模型和运行库，并在 READY 前对四个会话各跑一次 dummy 推理预热（把 ORT 首跑懒初始化移出认证关键路径，见 [`performance-baseline.md`](../performance-baseline.md) 实验 8）；`AUTH_REQUEST` 到达后才允许枚举并激活摄像头，认证成功、失败、超时或通信异常后都退出。不存在提前打开摄像头的配置开关。父进程通过私有继承匿名管道取得最多三条 512-D embedding 并在本地匹配 SID；`users.dat`、DPAPI 密码和 `AUTH_SUCCESS` 构造始终留在父进程。

四个 ONNX 会话也不在已解锁桌面常驻：锁屏事件先异步预加载 worker；认证请求只在事件延迟或丢失时等待其启动。一次 worker 只处理一个请求，失败/超时会在锁屏期间后台补建，成功则等待下一次 `AUTH_REQUEST`；Windows `SESSION_UNLOCK` / `LOGON` 关闭未使用 worker。父进程用模型使用租约避免在认证控制通道未完成时并发终止 worker。模型始终按完整包发布，任一完整性校验或初始化失败都不允许部分认证。

模型会话与 ONNX 运行时资源由 worker 私有持有：每个 worker 完成一次认证后进程退出，因此 DirectShow `BindToObject` 遗留的驱动 `EtwRegistration` 和 ORT/Windows 堆高水位随进程一并回收。成功终态会先回传父进程，worker 随即由系统终止；因此不会把可能较慢的 DirectShow `Stop` 放在解锁关键路径。父进程仍使用 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 作为孤儿 worker 的最终保护。子进程内部可使用共享全局 `Ort::Env`/线程池和 `DisablePerSessionThreads()`；这些对象不会进入常驻父服务。私有协议对配置、request ID、binding 顺序、embedding 维度/有限值/L2 范数做强校验，完整线格式见私有契约。

生产 EXE 位于 Program Files 时，DataPath 只允许解析到 EXE 目录；异常重定向会使服务拒绝启动。非生产 standalone 允许 EXE 目录、ProgramData 和显式开发 hint，完整顺序见 `DEVELOPMENT.md` 的路径表。

## 认证主循环

认证算法只实现于 `AuthPipeline`：生产服务由一次性 worker 调用，standalone 由
`FaceService::ProcessAuthRequest` 在本进程调用。两条入口共用以下不变量，不能再复制一份
独立的检测/PAD/绑定循环：

1. 无注册账号直接返回错误。
2. 双 MiniFAS 活体检测强制启用；没有可切换的方法或关闭开关。
3. 延迟初始化摄像头并做自适应曝光预热（`exposure_warmup.h`：按帧序号去重采样均值亮度，稳定窗口即通过，2–10 帧）。
4. 在同一融合循环中完成 SCRFD 检测、双 MiniFAS PAD 和身份一致性。
5. 所有计入的 PAD 帧都必须通过。
6. 首个身份锚点锁定 SID，中间和末尾绑定帧必须匹配同一 SID。
7. 非绑定帧只用 bbox IoU 维持人脸连续性。
8. 末帧身份锚点同时是最终身份门，不再追加独立 post-liveness embedding。
9. 成功后才构造 `AUTH_SUCCESS`；失败路径不得泄漏或遗留明文密码。

全局认证时限和 PAD 独立窗口同时生效。无人脸、持续 PAD 拒绝还有挂钟式快速失败，避免低频 CPU 上按帧计数造成等待时间漂移。

主循环按单预取槽软件流水线调度：PAD(N) 在 async 线程跑时，主线程预取帧 N+1 的抓帧 + SCRFD 检测，判定逻辑与逐帧顺序不变，只有调度顺序改变。帧 pacing 由显式抓帧守卫保证（相邻已计帧采集间隔 ≥60 ms 且帧序号严格递增，锚点在预取前乐观前移到当前帧），每轮结束记录实际最小采集间隔日志。

## 摄像头

所有运行期和开发工具均使用 `webcam_capture_dshow.*` 的 DirectShow 实现：服务 worker、standalone、Enrollment 预览与 PAD 标定共享同一条相机路径。worker/standalone 在 MTA 中运行，Enrollment 预览复用 WebView2 UI 的 STA；实现必须只在自身初始化的同一线程调用 `CoUninitialize`。设备的**枚举与图骨架**（FilterGraph/SampleGrabber/Null Renderer，注册表级读取）在 worker 锁屏预载期完成（`WebcamCaptureDS::Preload`，不激活设备、不亮 LED）；**设备激活**（BindToObject/连接/Run）只在 `AUTH_START` 或用户启动预览后。修改相机实现时必须验证 Session 0 和 Desktop 预览两种宿主。

## ONNX 管线

| 类 | 正式模型 | 输入/输出 | 角色 |
|---|---|---|---|
| `OnnxDetector` | `det_10g_gnkps.onnx` | 512×512 → bbox + 5 点 | SCRFD 检测 |
| `OnnxRecognizer` | `w600k_r50.onnx` | 112×112 → 512-D L2 embedding | 身份识别 |
| `OnnxAntiSpoof` | `MiniFASNetV2.onnx` + `MiniFASNetV1SE.onnx` | 2.7×/4.0× crop → 融合分数 | 静默 PAD |

SCRFD 5 点经 [`face_align.h`](../../face_service/face_align.h) 做相似变换和偏航角估计。不存在 dlib 检测器、68 点模型或当前 128-D 识别器。

生产 worker 与 standalone 都在构造 ONNX Session 前校验四个模型的 SHA-256，C++ 常量集中在 `common/model_hashes.h`；安装器还会在提取前后独立校验同一组模型。模型已在当前工作区准备完成，普通构建不要重新下载或量化。

模型生命周期状态为 `Unloaded → Loading → Ready/Failed`，其中 `Ready` 表示私有 worker 的 Hello/Init/Ready 握手已经完成。`HandlerEx` 只提交加载/释放请求，构造/关闭由长期生命周期线程完成；`AUTH_REQUEST` 是锁屏通知之外的加载兜底。`CONFIG_RELOAD` 在桌面已解锁时只更新下一次 worker 的配置；锁屏时会以配置代次替换 worker，确认新 worker 已就绪后才回复成功。`service.log` 记录父进程资源和成功终态计时，`auth_worker.log` 记录 worker 的模型加载、预热选择、相机/PAD 过程以及失败终态；成功路径不在 worker 内同步写终态日志。身份不匹配的失败轮会补齐诊断字段：父进程 WARN 附"closest identity distance=…, threshold=…"(该轮最近身份距离，`FindBestIdentity` 出参收集)，worker 的 unknown-face 快速失败与成功终态行附"width=… px, face luma=…"(首次身份 miss / 末次绑定帧的人脸宽与亮度)，用于区分"差一点过阈值"与"完全不是本人"、以及光照域差问题。`CameraLifecycleTest` 用 DirectShow 的 inproc/child 模式确认驱动与隔离边界；`AuthWorkerProtocolTest` 验证私有协议与畸形输入；`AuthWorkerLifecycleTest` 覆盖 supervisor 故障、Job 清理和 100 次模拟认证退出；`CredentialStoreTest` 覆盖 identity-only 匹配、最终单次解密和仅 V4 的 fail-closed 读取。

## 当前认证常量

这些值用于定位行为；修改前必须回到代码和标定文档复核。

| 参数 | 当前值 | 事实源 |
|---|---:|---|
| 512-D 匹配默认阈值 | 0.80 | `AppConfig::match_threshold` |
| 匹配安全带 | [0.70, 1.00]，越界回退 0.80 | `EmbeddingThresholdForDim` / config normalize |
| best/second-best 拒绝比 | `>= 0.75` | `CredentialStore::FindMatch` |
| 双 MiniFAS 权重 | 0.5 / 0.5 | `OnnxAntiSpoof` |
| PAD 阈值 | 0.28（UI 可调 0.15–0.50，标定点 0.281） | `AppConfig::anti_spoof_threshold` |
| 服务相机后端 | DirectShow | `webcam_capture_dshow.*` |
| 锁屏摄像头预热 | 禁止；只在 `AUTH_START` 后打开 | `RunAuthenticationWorker` |
| worker 电源策略 | `AUTH_START` 起解除 EcoQoS 限流（EXECUTION_SPEED opt-out；A/B 实测慢机防 P 核被压至 1466–1833/2200 MHz，插电也会发生） | `RunAuthenticationWorker` |
| 私有管道轮询定时器精度 | 认证窗口 1 ms（worker 自 READY 后空闲环起、父服务自 `StartAuth` 写入起；默认 ~15.6 ms 节拍下 `Sleep(1)` 实睡一拍，双机埋点实测拾取等待合计开发机 ~40 ms/慢机 ~20 ms；Win11 起提精度仅作用于本进程） | `ScopedTimerResolution`（`timer_resolution.h`） |
| PAD 计帧 | 5/5 全过 | `liveness_types.h` |
| 已计帧采集间隔 | ≥60 ms + 帧序号严格递增（显式抓帧守卫） | `AuthPipeline::Run`（`auth_pipeline.cpp`） |
| 全局认证时限 | 15 s | `m_authTimeoutSeconds` |
| PAD 窗口 | 8 s | `ProcessAuthRequest` |
| 全程无人脸快速失败 | 2.5 s | `ProcessAuthRequest` |
| 持续 PAD 拒绝快速失败 | 2.0 s，自首次检测到人脸起算 | `LivenessTiming`（`liveness_types.h`） |

`FaceService.h` 中 `m_matchThreshold = 0.30f` 的成员初始化器会在 Initialize/reload 立即被配置覆盖，不是运行时默认值。

## 性能决策

当前基准显示 CPU 频率是主要决定变量，多帧全嵌入在降频机器上不可行。除非有新的可复现实验，不做逐机阈值/帧数调优，也不要恢复独立 post-liveness embedding。

迁移前的性能实验与嵌入优化见 [`performance-baseline.md`](../performance-baseline.md)；当前 worker 性能、资源和后端决策见 [`auth-worker-migration-completion.md`](../auth-worker-migration-completion.md)。阈值修改必须走 [`threshold-calibration.md`](../threshold-calibration.md) 的标定流程，不能凭主观体验调整。

## 错误与日志

- 模型缺失或 PAD 推理异常：fail-closed；模型加载/完整性失败按类别输出锁屏文案（检测/识别/活体完整性 vs 普通加载失败），单一来源 `face_service/model_failure.h`，worker 与 standalone 共用
- 不可信 DataPath：拒绝启动
- 无人脸/PAD/身份不一致：`AUTH_ERROR:message`
- 全局时限耗尽：`AUTH_TIMEOUT`
- 客户端中途断开：停止认证并清除敏感数据

生产日志：`<DataPath>\log\service.log` 和 `<DataPath>\log\auth_worker.log`。standalone 默认启用 Debug；初始化成功只表示公开管道和 worker 生命周期线程已启动。锁屏/登录界面应出现 `Authentication worker ready in ... ms`；成功认证时 `service.log` 应先出现 `Credentials sent ...`，随后记录 `Authentication worker timing ... cleanup=...`，子进程应已退出。已解锁时父进程只保留公开管道、凭据库和基础线程。连续循环要比较父进程句柄/线程/private bytes/working set 是否稳定，确认每轮 worker PID 均已退出且无孤儿 Job，不能再以子进程的临时高水位判定父服务泄漏。

## 修改检查表

- 认证顺序变化：检查 IPC、CredentialStore、密码清零和性能基准
- 相机变化：验证 DirectShow 的 child 生命周期、Session 0 与 Desktop 预览
- 模型生命周期变化：验证锁屏预加载、密码解锁释放、快速锁定兜底、连续循环和服务停止
- 模型变化：同步 C++ hash、Go 资源校验、下载/转换脚本和标定文档
- 阈值变化：更新默认配置、钳制、安装器迁移和标定结论
- 最低验证：Release 构建 + 三个 worker/凭据测试；涉及相机先跑 DirectShow child 生命周期，再补 Session 0/锁屏验证
