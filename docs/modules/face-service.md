# 人脸认证服务

本文说明 `face_service/` 的进程边界和认证约束。实现入口为 [`FaceService.cpp`](../../face_service/FaceService.cpp)。公共 IPC、worker 私有 IPC 和数据库格式分别以 [`IPC 契约`](../contracts/ipc.md)、[`worker IPC 契约`](../contracts/auth-worker-ipc.md) 和 [`users.dat 契约`](../contracts/users-dat.md) 为准。

服务目录保留生命周期、认证流程、worker、管道和学习调度。相机、模型推理、对齐与曝光控制由共享目录 [`vision/`](../../vision/) 提供；凭据数据库与匹配由 [`storage/`](../../storage/) 提供。服务和注册程序链接同一份静态库实现，各进程仍独立持有实例和运行状态；构建依赖见 [`BUILD.md`](../BUILD.md#c-构建)。

## 职责与生命周期

服务父进程负责 DataPath、配置、`users.dat`、公开命名管道和身份匹配。锁屏期间异步预载一个 `-auth-worker`，worker 持有 ONNX 模型并在认证请求后打开摄像头，完成一次请求即退出。父进程不持有 ONNX Session，也不会在认证前打开摄像头。

worker 预载诊断在 `auth_worker.log` 输出当前 Session 与显式电源限流标志、三个模型组各自的 SHA-256 校验/初始化耗时，以及模型加预热、相机图骨架和总预载耗时。电源标志不等于实际 CPU 频率，也不足以排除自动调度限制；桌面探针不能替代锁屏 Session 0 测量。桌面加载实验见 [`sface-performance.md`](../work/completed/sface-performance.md)。

认证流水线的曝光诊断在本轮退出时汇总输出：`Exposure phase timing` 拆分预热结束后的检测收集、失效 seed 重抓、参数重放后的稳定等待、触发诊断、调节入口和状态交付；`trigger_query` 单列诊断查询耗时，`query_before_seed_wait` 表示查询是否安排在检测收集前。此时 `seed_wait` 只计查询之后的剩余等待，`trigger` 不再包含提前执行的查询。`Exposure sensor timing` 按重放、重放稳定检查、触发诊断、调节及认证循环分组，记录每类传感器回调的调用次数、失败次数、累计及最大耗时。计时覆盖宿主回调全程（包括 DirectShow QI/Get/GetRange/Release），不是单个驱动内部操作；不记录画面、向量或凭据。分项和调用耗时有包含关系，不能重复相加。

worker 的纯诊断快照通过一次 `GetExposure` 同时读取曝光值和模式；宿主未提供合并回调时使用原来的独立读取。未发生参数重放写入且有待收集检测时，在相机原 COM 线程上先读取诊断参数，与已有异步 SCRFD 重叠；发生重放写入时，仍在稳定等待/重测之后读取。快照仅用于日志，不供调节器作控制判断；调节器继续读取当前模式，曝光诊断本身不改变检测帧选择、新鲜度检查、计帧间隔与认证规则。阶段对照与故障路径检查见 [`sface-performance.md`](../work/completed/sface-performance.md)。

预热循环先判断当前样本是否打开稳定门，再决定是否启动当前帧检测。在无控制重放写入的稳定窗口内，自动曝光策略已确认 AE 活跃，或在预热前通过当前传感器回调确认手动曝光时，若已有来自紧邻上一预热样本的已完成人脸检测，年龄不超过 400 ms，且旧帧与当前帧在旧人脸框内的亮度均处于 50–120、差不超过 10，则复用已有 seed；模式未知、查询失败或其他条件不满足时保留原来的最新或待完成检测流程。手动模式查询在相机原 COM 线程上执行，不使用诊断快照作控制判断。亮度检查先按配置旋转当前帧，后续过期丢弃、控制写入后失效、身份绑定、PAD 和 60 ms pacing 仍执行。异步检测 future 在输入帧释放之前收敛，包括取消退出路径。

`Exposure phase timing` 的 `preparation` 记录预热前的传感器准备（策略判断、缓存重放和手动模式确认），与首帧等待窗口存在重叠，不能额外加到 pipeline 首帧时间上。`seed_reused` 标识上述已完成结果复用分支，`seed_seq` 和 `seed_age` 记录预热门后、过期丢弃和曝光调节之前的 seed 选择；不代表该帧最终已计入认证。自动曝光试用包记录见 [种子调度试用](../work/completed/sface-performance.md#ready-seed-trial)，手动曝光扩展与首帧诊断见 [当前优化记录](../work/in-progress/unlock-performance.md)。

种子调度试用包安装后的 7 次已 ready 的 Session 0 请求全部由 Windows 确认成功，6 次走新增复用分支，全部选择第一帧且 seed 剩余等待为 0 ms；pipeline 中位数 430.3 ms，CP 请求到 Windows ReportResult 为 619 ms。与前批 7 次比较不是受控 A/B，完整差值不能全部归因于调度，首轮相机初始化慢尾仍在。逐轮和限制见 [安装后记录](../work/completed/sface-performance.md#ready-seed-installed)。

ONNX Runtime 的 Warning/Error 通过自定义 Env 日志回调写入宿主的项目日志，使用原有分级刷盘；回调不向 ORT 的 C 接口抛出异常。保留默认 Warning 级别及原有模型加载顺序，避免无控制台 Session 0 中默认 `std::wclog` 输出让 SFace 初始化多付约 1 秒。性能对照与输出一致性证据见 [`sface-performance.md`](../work/completed/sface-performance.md)。

```text
Initialize → 校验路径/加载配置和数据库/启动管道与生命周期线程
锁屏       → 异步预载 worker 与模型
AUTH_REQUEST → worker 打开相机并认证 → 父进程匹配 SID、构造认证结果
解锁/登录   → 关闭未使用的 worker
```

认证成功、失败、超时或通信异常后，worker 都会退出；服务在锁屏期间按需补建。模型不完整或加载失败时拒绝认证。父进程通过私有管道接收最多三条 SFace embedding，密码解密及 `AUTH_SUCCESS` 构造留在父进程。worker 生命周期和消息字段见私有 IPC 契约。

生产服务路径必须解析到 EXE 所在安装目录，不可信重定向会被拒绝。standalone 的开发路径规则见 [`DEVELOPMENT.md` 路径表](../../DEVELOPMENT.md#sec-paths)。

## 认证流水线

`AuthPipeline` 是 worker 与 standalone 共用的认证实现。主要约束如下：

1. 没有已注册账户时直接失败；双 MiniFAS 活体检测始终启用，任何模型错误都按失败处理。
2. 摄像头启动后先做曝光预热。人脸亮度出带时，按设备能力调节曝光和增益；有增益时优先用增益细调，无增益时优先保持或恢复驱动自动曝光。指定 `DevicePath` 时必须使用该设备，找不到即失败，不得回退到其他相机。
3. 调节器须等待可观察的亮度响应与稳定；没有响应证据至少等待 900 ms，持续漂移或未稳定时不确认结果。稳定调节结果可写入 DataPath 根目录的 `camera_tune.state`；取消、丢脸或未稳定不得覆盖旧记录。缓存使用 `exposure`、`gain` 及可选的 `exposure_manual=0|1`：数值相同但模式丢失时也要恢复模式，已处于自动模式时不把缓存中的自动曝光读数锁成手动值。旧两行缓存继续按数值重放，后续调节保存时补充可查询的模式。接管自动曝光后亮度已落入 50–120 时不再为了增益余量继续上调；亮度余量允许时回收过长曝光，但不能低于人脸亮度底线。
4. 无脸且画面大面积饱和时可以有限下调曝光以恢复检测。无脸且中央三分之一区域中最暗四分之一样本的平均亮度不高于 30（避免亮背景淹没较小的暗脸，同时排除少数孤立暗像素）时，先确认暗光稳定，再有限提高增益；自动/未知曝光模式先固定当前曝光值，增益不可用、耗尽或无响应时再按单个驱动步长提高曝光，并受现有曝光上限约束。每次暗光恢复最多写入两次（包含模式接管），该中央暗部亮度达到 50 后停止无脸试探；每次写入等待响应与稳定，出脸后另行确认人脸亮度稳定，再交回人脸优先调节。无脸恢复本身不保存参数。认证在预环或首个计帧前可尝试暗光恢复，每轮最多进入一次；取消、断开及全局 5 秒截止时间继续生效。注册预览使用同一调节逻辑，并在采集期间跳过后台调节。
5. 预环后若首个计帧前检测到迟到入场或亮度漂移，可纠正曝光，最多两次并丢弃触发帧重抓。计帧开始后不再修改传感器参数；纠正期间暂停并平移 PAD 计时，全局 5 秒时限继续计时。
6. 每个计入的帧都必须通过双 MiniFAS PAD。默认 5 帧，首、中、末绑定帧必须匹配同一 SID；非绑定帧以 bbox IoU 保持连续性。`fast_unlock` 模式使用 3 帧且每帧绑定。相邻计帧采集至少间隔 60 ms，帧序号必须递增。等待新序号时也检查取消、客户端断开、认证时限及 8 秒 PAD 窗口；相机持续返回旧缓冲帧不能绕过这些退出条件。
7. 最佳身份须同时通过距离阈值和最佳/次佳比门控。成功后才生成认证结果；失败时清除敏感数据。

身份绑定返回 `Retry` 时，该帧不计入认证证据，并启动独立的 2 秒身份匹配失败计时；再次接受身份绑定时重置。即使此前已有活体帧通过，连续重试仍在该预算耗尽后的下一次身份拒绝时失败，不再只等 8 秒 PAD 窗口。首绑前曝光纠正会暂停该计时；无帧、无脸或 embedding 计算失败不会单独启动它。已有部分身份绑定后持续匹配失败，提示调整位置或光照后重试；窗口结束时若已计入的 PAD 帧全通过但仍有待完成的身份重试，也按身份未完成报告。日志用 `Authentication evidence incomplete` 分别记录身份绑定数和 PAD 通过/计帧数，避免将证据不足统一记成活体拒绝。

每轮已有的 embedding 范数日志附带 `Binding capture sequence`：记录实际绑定尝试次数、相邻尝试重复序号次数及首末序号，用于区分持续身份不匹配和重复读取相机旧缓冲帧；不记录画面或向量。

预热与传感器控制实现见 `exposure_warmup.h`、`face_gain_tune.h` 和 `auth_pipeline.cpp`。曝光探针的使用方法见 [`BUILD.md`](../BUILD.md)。

当前落带参数和已确认调节进度的保存发生在最终身份/PAD 判断前；参数的有效性以曝光测量和稳定依据为准，认证失败本身不否定参数有效，认证成功也不保证参数适合下一轮的新光照。环境切换记录、重复恢复的待验证机制及后续提速候选见 [曝光问题与解锁提速方向](../work/in-progress/exposure-issues-and-speedups.md)。

调节写入因丢脸或样本预算耗尽而未确认时，共享调节器保留本次写入的亮度背景、方向和参数快照。认证在首个计帧前遇到落带人脸时最多接续复核一次：独立稳定门只采纳严格递增的新帧，使用原认证截止时间，复核前后参数及可查询的手动模式须与快照一致；通过后才更新缓存。复核不写硬件、不计身份/PAD 帧，不依赖认证结果，失败不得通过单帧落带分支补存。无脸恢复的场景亮度不能证明人脸响应，须按无响应回退条件重新确认人脸。正常稳定轮次不进入接续复核。

## 摄像头与模型

SFace 正式运行导出已移除 174 个 initializer graph inputs，保留权重、节点和真正的图像输入；因此不再产生这组 ORT WARN。运行哈希由 `common/model_hashes.h` 固定，与原始上游哈希不同；来源及转换记录见 `third_party/models/sface/provenance.json`。已有 V6/SFC1 人脸库保持兼容，本次不改识别阈值或要求重录。原生输出与旧库对照、初始化/推理取舍见 [`sface-performance.md`](../work/completed/sface-performance.md)。

所有运行期相机路径使用 DirectShow。锁屏预载只枚举设备并准备图骨架，不激活设备；设备仅在认证请求或用户启动预览后激活。worker 在 MTA，Enrollment 预览在 UI STA；DirectShow COM 初始化和释放必须留在同一线程。改动相机生命周期时要覆盖 Session 0 与桌面预览。

图骨架可预载设备无关的软件 AVI Decompressor。激活后，已读取的当前输入类型为 YUY2 时优先直接连接 capture → AVI Decompressor → RGB24 SampleGrabber；转换器不可用、输入为其他类型或直接连接失败时，删除可选滤镜并使用原 Intelligent Connect。直接连接失败须先断开两段连接的双方；清理失败则初始化失败，不在残留连接上重试，也不切换设备。`DirectShow webcam initialized` 日志用 `(AVI direct)` 或 `(Intelligent Connect)` 标识实际路径。预载不绑定设备、不连接相机引脚或启动采集。

`Auth pipeline first valid frame ... after camera ready` 的实际计时起点是 `AuthPipeline::Run` 内的 `pipelineStart`，终点是首次成功 `grabFrame` 返回；它包含前置传感器准备、轮询和 RGB 拷贝，不直接测量 `Run()` 返回到流线程首回调。当前 warmup 仍用 10 ms 轮询。桌面首帧复审发现事件取帧候选，用户放弃其约 5–6 ms 收益；MF 桌面首样本成绩不足以推翻其历史完整 Session 0 回归，继续保留 DirectShow，见 [首帧复审](../work/completed/sface-performance.md#first-frame-review)。后续桌面调查发现预载同一软件转换器并直接连接可使初始化约省 21–23 ms；用户同意试用后已实施上述兼容回退，并确认安装后的 4 次 Session 0 锁屏均使用直接连接且成功。其初始化中位数 164.2 ms，完整解锁 648.5 ms；与早先小批次比较不是受控 A/B，长期收益仍待验证，见 [试用实现及锁屏记录](../work/completed/sface-performance.md#camera-direct-trial)。

| 模型 | 输入/输出 | 用途 |
|---|---|---|
| `det_10g_gnkps.onnx` | 512×512，框与 5 个关键点 | SCRFD 检测、相似变换对齐 |
| `face_recognition_sface_2021dec.onnx` | 112×112，128 维 embedding | SFace 身份识别 |
| `MiniFASNetV2.onnx`、`MiniFASNetV1SE.onnx` | 2.7× / 4.0× 人脸裁剪 | 双模型融合 PAD |

worker 和 standalone 在创建 Session 前校验四个模型的 SHA-256；C++ 哈希见 `common/model_hashes.h`，安装器提取时也会校验。普通构建复用现有模型，不要重新下载或量化。

worker 的模型状态为 `Unloaded → Loading → Ready/Failed`。加载、释放由生命周期线程串行处理；配置重载会替换锁屏 worker，并在新 worker 就绪后确认。服务与 worker 分别写 `service.log`、`auth_worker.log`；排障步骤见 [`运维手册`](../operations/operations.md)。

`DirectShow first frame timing` 单独记录采集启动边界：`run_call` 为 `IMediaControl::Run` 调用耗时；`run_to_callback` 从调用 Run 前到首个有效 BufferCB 入口；`callback_copy` 包含该回调锁等待、缓冲分配和拷贝；`ready_to_grab` 从首缓冲写好到应用首次成功取帧取得锁并准备拷贝；`grab_lock_wait` 为该次取帧的锁等待；`grab_copy` 为 RGB 转换拷贝。回调只记录 QPC 时间戳，日志在首次成功取帧释放锁后输出，每次 Run 最多一条。`run_call`、`grab_lock_wait` 与其他分项有重叠，不能全部相加；这些是采集链路边界，不直接指认固件内部成本。诊断只记录时间，不保存画面或改变采集调度。

## 当前关键值

| 项目 | 当前值 |
|---|---|
| SFace 匹配阈值 | 默认 1.00，试用范围 [0.70, 1.00]；非生产标定 |
| 最佳/次佳比门控 | 0.75 |
| PAD 融合与阈值 | 双 MiniFAS 各 0.5；默认 0.28（标定点 0.281） |
| PAD 计帧 | 默认 5；`fast_unlock` 为 3 |
| 全局认证时限 | 5 秒共用预算，包含模型等待、相机与曝光准备；PAD 原独立窗口 8 秒受总时限约束 |
| 无脸 / 持续 PAD 拒绝快速失败 | 2.5 秒 / 2 秒 |
| 连续身份匹配重试预算 | 2 秒；身份接受时重置，与 PAD 通过次数无关 |

阈值的标定证据见 [`threshold-calibration.md`](../work/completed/threshold-calibration.md)；SFace 试用限制见 [`sface-trial.md`](../work/in-progress/sface-trial.md)。

## 故障处理与修改检查

- 模型缺失、哈希不符、PAD 推理异常或 worker 通信失败：拒绝人脸认证，保留 Windows 密码登录。
- DataPath 不可信：服务拒绝启动。
- 无人脸、PAD 失败或身份不一致：返回认证错误；全局时限耗尽：返回超时。
- 修改认证顺序时检查 IPC、凭据匹配和密码清零；修改模型时同步哈希、安装器校验和标定文档；修改相机时覆盖 child 生命周期、Session 0 和桌面预览。

最低验证命令见 [`BUILD.md`](../BUILD.md)。相机、Credential Provider 和锁屏行为还需按实际设备验收；自动化测试不能替代实机认证。
