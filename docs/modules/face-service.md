# 人脸认证服务

本文解释 `face_service/` 的跨文件行为和修改边界。实现入口是 [`face_service/FaceService.cpp`](../../face_service/FaceService.cpp)；Credential Provider 公共协议、worker 私有协议与持久化分别见 [`IPC 契约`](../contracts/ipc.md)、[`auth-worker IPC 契约`](../contracts/auth-worker-ipc.md) 和 [`users.dat 契约`](../contracts/users-dat.md)。

## 责任边界

`FaceLoginService.exe` 是自动启动的 Windows 服务，也支持 standalone 开发模式。它负责：

- 安全解析 DataPath
- 父进程加载配置、凭据数据库并维持公开命名管道
- 锁屏时预加载一个私有认证子进程；子进程独占四个 ONNX 模型和摄像头
- 检测、PAD、身份一致性和匹配
- 认证成功后的模板渐进学习（`TemplateLearner`，设计见 `docs/progressive-learning-v2.md` §3；含 `LEARNING_STATUS` 只读内存快照查询，供控制台学习状态视图）
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
3. 延迟初始化摄像头并做自适应曝光预热（`exposure_warmup.h`：按帧序号去重采样均值亮度，稳定窗口即通过，2–10 帧）。预热后若检测框亮度在带外（<50 暗或 >120 过曝，触发带与目标带 50–90 之间只留一步过冲容差；旧 180 触发线曾把 90–180 留成治理真空——P2 修复让曝光首次爬得上去后，-2 落带 89 的传感器状态 40 秒内随晨光漂到 178，2026-09-13），运行 face luma 驱动的曝光调谐（`face_gain_tune.h`：先步进 `IAMCameraControl_Exposure` 再以 `VideoProcAmp_Gain` 细调。曝光是 ~2× 一档的粗执行器：上行步长为半剩余跨度向上取整不小于驱动 granule，下行单 granule 细步；**步进循环停车线=触发带 [50,120]**——目标带 50–90 比一个量子还窄，控制器必须允许 90–120 落点，否则在 -2↔-4 间永久乒乓；**上行封顶 -4（2⁻⁴=62.5ms≈帧守卫 60ms）**——再亮一档 (-3=125ms) 相机掉到 ~8fps：计帧间隔被顶到 95–118ms、曝光预热不再收敛，9-13 晚连续 6 轮 ~1.0s 的根因；封顶后的亮度缺口由增益承担，增益打满/判死仍暗才允许曝光破帽（极端暗房：慢但能过）；**每步测量是"900ms 时间底 + 相邻采样一致(≤6)"的稳定门**——该驱动曝光写入生效延迟 ~0.5–1s（读数滞后一步、上行曾把正常曝光误判死旋钮去拉增益 39→48→54→58，2026-09-13 极限环 -2(≈145)↔-4(≈47) 的根因）；**增益是细调执行器且方向无关**：曝光落定后 <50 抬、>90 降，用于桥接"任何曝光档都进不了带"的环境光区间（实测 -3≈45–54 vs -2≈137–165）并回收持久化的暗场高增益；**暗侧贴线触发（luma ≥ 带下限×0.75）增益先行**——一个曝光档是 2× 光子，对几个单位的缺口是过冲，且落点把帧率永久锁进低档（9-13 晚拔插轮 3 单位缺口 -5→-4，16fps 每轮 +180ms），增益放大不占帧时间，补不进带再走曝光路径（封顶/破帽规则不变）；旋钮两步无效即判死弃用，触发测量复用预热种子检测，亮场景零额外推理；**手动曝光/增益在驱动内跨 graph 保存，但设备断电（闲置 >~9s 再开）会间歇性静默回默认**（9-13 晚实测：-3 写入读回 -5、gain 22 读回 0）——落带内或本轮调节结果经稳定门确认后的 {曝光,增益} 写入 DataPath 根目录 `camera_tune.state`，即使预算耗尽仍未落带也保存进度；取消、丢脸、未通过稳定门时不覆盖旧记录（2026-09-15：暗处每轮增益 50→58 后亮度仅 43–49，旧逻辑因未到 50 拒绝保存，下轮重放又主动 58→50，导致连续每轮约 4 秒；现改为跨轮续调），worker/console 开相机先重放（等值不重写；worker 在预热前、console 在模型加载前重放，写入生效延迟被该窗口吸收；**重放有写入且触发首测出带时，worker 等完 ~900ms 生效窗口用新帧重测一次再决定是否调谐**——在旧读数上调参会与重放叠写、误判死旋钮，2026-09-13 21:59 一次 4.3s 轮的根因）；上行靠多轮认证棘轮到范围顶；背景亮/小脸欠曝时驱动 AE 只平衡全帧平均——这是 face luma 调谐存在的根因（曾试的 BLC 与识别器侧 low_light_enhance 均实测不足，分别于 9-11/9-13 删除））；调成功则预热种子作废重抓。注册控制台预览在 `StartPreview` 跑同一调谐，且帧线程带**流式曝光看护**：检测到人脸时每 3s 测一次框亮度，**连续两个周期出带**才触发一次预算内调谐（单帧读数随人脸占比抖动，10s 冷却；采集进行中跳过，保证一次采集看到一个稳定域），流式期间的 ambient 漂移不再无人治理。相机枚举：config 配置了 DevicePath 时该路径是权威选择——枚举不到（开机初期 USB 重枚举窗口内设备短暂消失，2026-09-13 实测）直接失败报"摄像头不可用"，绝不静默绑定"第一个设备"（曾把同机另一台相机放上认证路径，连吃空场景快速失败）；仅未配置相机的宿主才取第一个设备。
4. 在同一融合循环中完成 SCRFD 检测、双 MiniFAS PAD 和身份一致性。
5. 所有计入的 PAD 帧都必须通过。
6. 首个身份锚点锁定 SID，中间和末尾绑定帧必须匹配同一 SID。
7. 非绑定帧只用 bbox IoU 维持人脸连续性。
8. 末帧身份锚点同时是最终身份门，不再追加独立 post-liveness embedding。
9. 成功后才构造 `AUTH_SUCCESS`；失败路径不得泄漏或遗留明文密码。

全局认证时限和 PAD 独立窗口同时生效。无人脸、持续 PAD 拒绝还有挂钟式快速失败，避免低频 CPU 上按帧计数造成等待时间漂移。

主循环按单预取槽软件流水线调度：PAD(N) 在 async 线程跑时，主线程预取帧 N+1 的抓帧 + SCRFD 检测，判定逻辑与逐帧顺序不变，只有调度顺序改变。帧 pacing 由显式抓帧守卫保证（相邻已计帧采集间隔 ≥60 ms 且帧序号严格递增，锚点在预取前乐观前移到当前帧），每轮结束记录实际最小采集间隔日志。



曝光速度策略与响应稳定门（2026-09-19，覆盖下方 9-15 及上文历史调谐顺序）：

- 查询曝光手动/自动标志；欠曝时若仍是 AE，先按当前值切手动并等稳定，再测增益响应。所有暗侧优先增益；增益有效但预算耗尽时保存稳定进度，不能据此误判必须加长曝光。增益耗尽/无效后曝光每次只升一个驱动步长，保留 -4 上行帽及极暗破帽回退。
- 带内亮度也检查慢曝光是否可回收。仅当下一档预测亮度仍高于 `minFaceLuma + settleTol`（默认56）才缩短，并实测验证；偏好停在 -5（1/32s，匹配请求的30fps），不为更短参数牺牲亮度。过亮纠偏仍可降到 -5 以下。帧率上限、检测及推理耗时仍限制真实收益，检测框不是身份/PAD 合格的替代。
- `FaceSettleGate` 对新帧亮度使用方向响应 + 稳定窗口 + 线性趋势：与写入前相比沿正确方向变化至少 `max(6, 5%基线亮度)`，随后至少4个样本、跨≥200ms、窗口范围≤3、斜率绝对值≤2亮度单位/秒，允许早于900ms完成。无响应证据至少等900ms，仍须满足窗口和趋势要求；预算耗尽、丢脸、取消、不稳定时不再写下一旋钮或保存。AE冻结没有确定方向，不走提前结束；无脸饱和恢复也保留原900ms稳定门。 两宿主参数重放后无写入前亮度基线，不能据此提前结束等待；需要继续调节时等待剩余900ms窗口再用新帧判断。worker重测无脸也替换旧种子，不能沿用重放前的人脸框。
- 外置 USB `vid_32e6&pid_9221` 桌面实测两轮（每轮曝光3次往返、增益3次往返，共24次写入；640×480；-5↔-6，gain22↔29）。第一轮亮度响应起点约157–178ms，写入前约5帧仍是旧亮度。独立第二轮用最终生产判据回放：9/12在356–648ms确认，1/12约926ms，2/12因漂移在40样本预算内未确认；确认值与末尾10帧中位数差不超过约2亮度单位。固定900ms在第二轮一段持续变化曲线上也会过早确认，因此本次没有把900ms简单替换成更短常量。
- 数据来自全帧/中央区域亮度，证明该设备执行器存在更短响应路径，不等同于人脸区域、不同曝光档位、Session 0或其他相机的标定。完整身份/PAD/锁屏端到端收益尚未测量。原始 CSV 位于开发产物 `build/exposure-response-20260919*.csv`；复测命令见 BUILD。测试结束恢复了原始曝光/增益值和模式，不改安装态 `camera_tune.state`。

无脸过曝恢复（2026-09-19）：暗场手动参数切到亮场可能使 SCRFD 完全丢脸。认证预热后、注册启动及预览看护共用 `RecoverSceneWhiteout`：无脸且 5×5 分区至少 20 区有 ≥70% 采样点亮度 ≥245，并且中央 3×3 全部满足时，才尝试恢复。初始确认及每次曝光写入后均等待 ≥900ms 和连续两帧状态一致；曝光每次下调一个驱动步长，最多 3 步，不低于驱动下限。恢复人脸后需框亮度稳定才交回人脸调节；无脸、不稳定、取消或抓帧失败均不保存恢复参数。认证恢复发生在活体计帧之前，丢弃旧种子；预览遵守 3s 看护/10s 冷却并在采集中跳过。分区判据是保守恢复启发式，避免中央暗脸加亮背景触发；局部过曝、正常白墙等边界仍需真实相机验证，不能保证所有无脸场景恢复。自动化已覆盖饱和判据、预算、稳定门、取消及交接，暗转亮真实锁屏/GUI 尚待实测。

首绑前迟到曝光纠正（2026-09-19 晚，17:45:46 真机失败修复）：预环调谐需要一张可测量的脸，触发帧无人脸（迟到入场、暗脸漏检）或启动后亮度漂移时，旧循环会在暗芯片上反复身份匹配直到攻击计时器杀轮（该轮 face luma 18、身份距离 0.989、4040ms，全程零调谐日志）。现认证循环在**首个计帧之前**（`totalChecked==0`：PAD/身份证据未开始，改传感器参数安全；该门也在 `OnFaceDetected` 之前，暗脸不会先把 2s 攻击时钟对准来救它的调谐）检出人脸但框亮度出带 [50,120] 时，以预算 **2 次**运行完整 `TuneFaceExposure` 纠正，触发帧作废重抓；计帧一旦开始（含 fast_unlock 首帧即绑定）不再改任何传感器参数。纠正期间 `LivenessTiming::Suspend/Resume` 把 8s PAD 窗口、2.5s 空场景、2s 攻击三个计时起点整体平移——传感器稳定是采集准备，不是活体评估；15s 全局认证时限仍走真实墙钟。两预算耗尽仍出带的人脸按旧行为继续匹配（身份/PAD 门不变）。standalone 宿主未接旋钮，该门自然失效（与预环调谐一致）。诊断日志族：预环固定输出 `Exposure tune trigger`（有无脸/框亮度/曝光值与手动模式/增益值——"为何跳过调节"从此可答）；AE 切手动输出 `AE takeover`；每步旋钮输出 `Tune step`（值变化/亮度变化/settled 状态与耗时）；每次等待输出 `Sensor settle`（early — response + plateau 或 no-response floor）或 `Sensor settle unconfirmed`（cancelled / face lost or grab failed / sample budget——17:46:32 那类"151ms unconfirmed、全旋钮 n/a"的原因即 AE 接管同值写入+settle 丢脸，此前不可见）。自动化：`LivenessTimingTest` 覆盖挂起平移（空场景钟/攻击钟/连续两次/无挂起 Resume no-op）；纠正门的循环级行为（预算、帧作废、仅计帧前）无自动化覆盖，暗光首脸失败复现场景需锁屏实测验收。

曝光调节收敛补充（2026-09-15）：真实暗光复测为 4.01s→1.69s→1.69s→0.72s，后续三轮约 0.72s；持久化回退已消除，但增益仍以小步 50→58→60→62 跨轮逼近，且贴着亮度 50 停车会被下一轮微小波动重新触发。现仅对增益上调使用本轮已稳定测量的步进响应估算下一步（最多外推前一步的两倍且不越驱动范围），有实测响应后不再受旧的每轮剩余范围比例限制；正在进行的增益纠偏以 50+稳定容差 6=56 为停车目标，当时初始亮度在 [50,120] 内仍不调节（9-19起增加有余量的慢曝光回收）。曝光档位的停车线、-4 上行帽、900ms 稳定门、活体与身份门均保持原规则。该收敛优化已通过模拟反馈回归，更新后的暗光锁屏收敛速度仍待实测。

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

模型生命周期状态为 `Unloaded → Loading → Ready/Failed`，其中 `Ready` 表示私有 worker 的 Hello/Init/Ready 握手已经完成。`HandlerEx` 只提交加载/释放请求，构造/关闭由长期生命周期线程完成；`AUTH_REQUEST` 是锁屏通知之外的加载兜底。`CONFIG_RELOAD` 在桌面已解锁时只更新下一次 worker 的配置；锁屏时会以配置代次替换 worker，确认新 worker 已就绪后才回复成功。`service.log` 记录父进程资源和成功终态计时，`auth_worker.log` 记录 worker 的模型加载、预热选择、相机/PAD 过程以及失败终态；成功路径不在 worker 内同步写终态日志。身份不匹配的失败轮会补齐诊断字段：父进程 WARN 附"closest identity distance=…, threshold=…"(该轮最近身份距离，`FindBestIdentity` 出参收集)，worker 的 unknown-face 快速失败与成功终态行附"width=… px, face luma=…"(首次身份 miss / 末次绑定帧的人脸宽与亮度)，用于区分"差一点过阈值"与"完全不是本人"、以及光照域差问题。`CameraLifecycleTest` 用 DirectShow 的 inproc/child 模式确认驱动与隔离边界；`AuthWorkerProtocolTest` 验证私有协议与畸形输入；`AuthWorkerLifecycleTest` 覆盖 supervisor 故障、Job 清理和 100 次模拟认证退出；`CredentialStoreTest` 覆盖 identity-only 匹配、最终单次解密、V4/V5 读取与标称角往返。

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
| PAD 计帧 | 默认 5/5 全过（绑定帧计帧 1/3/5）；config `fast_unlock`=3 计帧全绑定（安全/速度用户自选，非标定结果） | `liveness_types.h` |
| 已计帧采集间隔 | ≥60 ms + 帧序号严格递增（显式抓帧守卫） | `AuthPipeline::Run`（`auth_pipeline.cpp`） |
| 全局认证时限 | 15 s | `m_authTimeoutSeconds` |
| PAD 窗口 | 8 s | `ProcessAuthRequest` |
| 全程无人脸快速失败 | 2.5 s | `ProcessAuthRequest` |
| 持续 PAD 拒绝快速失败 | 2.0 s，自首次检测到人脸起算（首绑前曝光纠正期间经 `Suspend/Resume` 平移暂停；8s 窗口与 2.5s 空场景钟同理） | `LivenessTiming`（`liveness_types.h`） |
| 首绑前迟到曝光纠正 | 预算 2 次，仅 `totalChecked==0` 且人脸亮度出带 [50,120] 时触发；15s 全局时限不暂停 | `AuthPipeline::Run`（`auth_pipeline.cpp`） |

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
