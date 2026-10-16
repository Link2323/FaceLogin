# 代码质量评估与改善待办

2026-10-15 对全库做了一次可读性 / 可维护性 / 可扩展性评估，覆盖 C++ 核心（face_service、common、vision、storage）、Credential Provider、注册界面（enrollment_app）、安装器（Go）、构建系统、文档与测试体系，证据定位到文件与行号。本文件收纳由评估产生的改善建议并登记完成情况；与 [`todo.md`](todo.md) 重叠的项只跟踪状态，详细描述以该处为准。全部清零后本文件移入 `../completed/`。2026-10-15 第二轮：第一梯队 6 项（#1/#3/#4/#5/#6/#9）完成后全库复评，更新结论并新增 #13-#19。

## 评估结论（2026-10-15）

| 维度 | 得分 | 主要依据 |
|---|---|---|
| 可读性 | 7.5 | 命名与"为什么"注释是标杆级（带日期的实测记录）；扣分在约 10 个超百行函数（最长 `AuthPipeline::Run` 约 395 行）、散落魔法数字、一处缩进错位、app.js 单作用域 1428 行。 |
| 可维护性 | 7 | 依赖方向单向、bool+FACELOGIN_ERROR 约定执行一致、IPC 协议单源、文档逐项对得上；扣分在 CLSID 四处 / DataPath 五处 / 模型哈希双份等靠文档约定同步的重复，credential_provider 近零自动化覆盖。 |
| 可扩展性 | 6 | AuthPipeline / TemplateLearner / AuthenticationResidency 三个接缝与模型版本化设计优秀；扣分在相机后端无抽象、凭据字段 ID 魔法数散布 5 处 switch、dispId 双手工表、新增宿主方法需 4 处同步。 |
| 综合 | 7 | 文档（9）、安装器（8.5）、构建（8）、C++ 核心（7~7.5）、credential_provider（6~8）、enrollment_app（5~7）、测试（7）。 |

整体判断：工程素养部分（文档、注释、安全纪律、测试策略）是强项；短板集中在"多份事实源"与"无抽象的可替换点"。第一梯队完成后可维护性预计可到 8；相机抽象与 dispId 单表化落地后可扩展性有望到 7.5。

## 复评结论（2026-10-15 第二轮，第一梯队完成后）

第一梯队 6 项经逐项核实全部落地且质量高：067b9c0 拆分逐路径等价、实测注释零流失；a6e58df 具名常量与全部使用点一一对应；efc0ed1 收口后全仓样板残留 grep 为零；#1 的 Go 测试实测通过且已做三方向破坏性验证。

| 维度 | 初评 | 复评 | 变化依据 |
|---|---|---|---|
| 可读性 | 7.5 | 8.5 | 超百行函数从约 10 个降到 7 个（最长 395→259 行；`AuthPipeline::Run` 89 行、worker 主线 116 行）；魔法数字主力具名；陈旧引用与缩进错位清零。app.js 1428 行、face_gain_tune.h 1185 行仍在。 |
| 可维护性 | 7 | 7.5 | CLSID/配置默认值有测试守护、service_client 三重复制收拢、CMake 样板收口；DataPath 五处与 config 默认值三处重复仍在，credential_provider 近零测试，新发现 bump_version.ps1 断链（#13）。 |
| 可扩展性 | 6 | 6.5 | 扩展性主线（相机抽象、dispId 单表、字段 ID 具名）本轮未动；#5 拆分后认证流水线各阶段可独立演化，小幅改善。 |
| 综合 | 7 | 7.5 | 面向「多份事实源」的第一梯队修复全部兑现。 |

模块复评：C++ 核心（face_service + common/vision/storage）8.5、credential_provider 7、enrollment_app 7、安装器 8.5、构建 8、文档 9、测试 8。契约文档抽查（users-dat / ipc / auth-worker-ipc 的关键常量）与代码逐项一致。

复评新发现已入清单：#13 bump_version.ps1 断链（`-Dry` 实测必失败，高）；#14 `DeleteFace` use-after-erase（真缺陷）；#15 enrollment_app 密码未清零（违反自定安全约束）；#16 app.js 两处同步 SetConfig 最长冻结 25 秒；#17 CP `g_cRefCount` 从未增减致 `DllCanUnloadNow` 失效；#18 `PrepareFrames` 252 行（#5 未覆盖的全库现存最长函数）；#19 低优先级杂项一批。

## 改善清单

难度标记沿用 [`todo.md`](todo.md)：**C** 局部小改动，**B** 单模块改造并补测试，**A** 跨模块/跨语言契约改动，**S** 多线程/COM/IO 生命周期重构。状态：未开始 / 进行中 / 已完成。

| # | 难度 | 建议 | 涉及位置 | 状态 |
|---|---|---|---|---|
| 1 | C | 跨语言常量一致性校验（CLSID、配置默认值） | `installer/FaceLoginSetup/` | 已完成（2026-10-15） |
| 2 | A | DataPath 解析统一 —— 已在 [`todo.md`](todo.md)「DataPath 解析统一」登记，以该处为准 | `common/data_path.cpp` 等 | 未开始 |
| 3 | C | 陈旧引用与缩进错位清理（onnx_models.h 注释部分已登记于 [`todo.md`](todo.md)「清理推理接口与离线标定的旧说明」） | 见下文 | 已完成（2026-10-15） |
| 4 | C | 魔法数字具名化 | 见下文 | 已完成（2026-10-15） |
| 5 | B | 拆分 `AuthPipeline::Run` | `face_service/auth_pipeline.cpp:45-439` | 已完成（2026-10-15） |
| 6 | C | CMake MSVC runtime 样板收口 | 根及各模块 `CMakeLists.txt` | 已完成（2026-10-15） |
| 7 | B | dispId 单表化与凭据字段 ID 具名 | `enrollment_app/WebviewHost.cpp`、`credential_provider/` | 未开始 |
| 8 | B | pipe_client 管道对测 | `credential_provider/pipe_client.cpp` | 未开始 |
| 9 | C | service_client 提取 request-ack 辅助函数 | `enrollment_app/service_client.cpp` | 已完成（2026-10-15） |
| 10 | A | 相机后端接口化（仅在确需 Media Foundation 或多相机差异时实施） | `vision/`、`face_service/auth_worker.cpp:336-355` | 未开始 |
| 11 | C | app.js 模块化（优先级最低） | `enrollment_app/app.js` | 未开始 |
| 12 | C | 同步原语不一致：收敛或在文档豁免 | `face_service/authentication_residency.cpp` | 未开始 |
| 13 | C | 修复 bump_version.ps1 断链（AGENTS.md 规则失配 + 尾注过时） | `scripts/bump_version.ps1:72-84` | 未开始 |
| 14 | C | DeleteFace use-after-erase 修复并补测试 | `storage/credential_store.cpp:489-490` | 未开始 |
| 15 | C | enrollment_app 密码局部变量清零（SecureWStringGuard） | `enrollment_app/` 3 个文件 | 未开始 |
| 16 | C | app.js 长超时 COM 调用防冻结（套用既有禁用按钮模式） | `enrollment_app/app.js:382-406、1354-1378` | 未开始 |
| 17 | C | CP 引用计数接入 g_cRefCount（DllCanUnloadNow 契约） | `credential_provider/dllmain.cpp:18、143` | 未开始 |
| 18 | B | 拆分 PrepareFrames（#5 延伸，全库现存最长 252 行） | `face_service/auth_frame_preparation.cpp:63` | 未开始 |
| 19 | C | 复评低优先级杂项 | 见下文 | 未开始 |

### 1 — 跨语言常量一致性校验（C）

CLSID 在 `credential_provider/resource.h:6-7`（DEFINE_GUID）、`credential_provider/dllmain.cpp:150`（字符串）、`credential_provider/FaceLoginProvider.cpp:173`（注册表路径）、`installer/FaceLoginSetup/internal/com.go:12` 四处独立硬编码，仅靠 AGENTS.md 文字约定同步；配置默认值在 `common/config_util.h` 与 `internal/config.go:10-11` 双份。

做法：扩展安装器现有测试（如 `resource_validation_test.go` 或新增），解析 `resource.h` 与 `dllmain.cpp`，断言 Go 侧 CLSID 一致；配置默认值同理。模型哈希部分不在此重复，见 [`todo.md`](todo.md)「模型 hash 跨语言单一来源」。

验收：`go test ./...` 通过新增断言；人为改动任一处 CLSID 测试变红。

### 3 — 陈旧引用与缩进错位清理（C）

- `vision/onnx_models.h:81-86` 注释仍写 640×640（实现为 512×512）——已在 [`todo.md`](todo.md)「清理推理接口与离线标定的旧说明」登记，以该处为准。
- 17 处源码/CMake/JS 注释引用 `progressive-learning-v2.md`（14 处带错误的 `docs/` 前缀、3 处裸文件名），实际路径为 `docs/work/in-progress/progressive-learning-v2.md`；如 `common/config_util.h:18`、`face_service/template_learner.h:33`、`tools/template_learner/CMakeLists.txt:2`。
- `face_service/FaceService.cpp:498-527` RELOAD_DB 分支缩进 12 空格（兄弟分支 8 空格），516-526 行 if/else 花括号错位，疑似合并残留。

### 4 — 魔法数字具名化（C）

- `face_service/auth_pipeline.cpp:205/297/323/351` 四处重复 `sleep_for(30ms)`（注释称 legacy 30 ms pace）。
- `vision/onnx_models.cpp:648-649` cropScale 2.7f/4.0f。
- `storage/credential_store.cpp:551-630` 1e10/1e9 哨兵与 0.75 比率门（带注释但未具名）。
- `vision/face_gain_tune.h:888-896` 245.0 剪裁阈值、70% 比率（`clipped*10 >= count*7`）、`clippedTiles >= 20`。

### 5 — 拆分 AuthPipeline::Run（B）

约 395 行、单函数约 10 个布尔结局标志（`auth_pipeline.cpp:119-139`）。按阶段（帧准备 / 活体 / 匹配 / 绑定）切为函数，结局标志合并为 enum。`face_service/auth_worker.cpp:169-446` 的 `RunAuthenticationWorker`（约 278 行）次之。

验收：`auth_pipeline_test`、`liveness_timing_test` 全绿；单函数不超过约 100 行。

### 6 — CMake MSVC runtime 样板收口（C）

tools/ 13 个 CMakeLists 与 5 个库目标各自重复 `MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"`。根 CMake 在 `add_subdirectory` 前统一设置 `CMAKE_MSVC_RUNTIME_LIBRARY`，credential_provider 的 /MT 例外单独覆盖。

验收：全配置构建通过，产物 CRT 链接方式不变（CP 静态、其余动态）。

### 7 — dispId 单表化与凭据字段 ID 具名（B）

- `enrollment_app/WebviewHost.cpp:391-434`（GetIDsOfNames 名称→id，38 项）与 `:498-606`（Invoke id→分支）是两份手工平行表，case 顺序已乱（32/33 插在 17 前，`:556-559`）。合并为共享名称→id 映射（宏或生成 enum），加 static_assert 保证两处等长。
- credential_provider 凭据字段 ID 0-4 为魔法数，散布 `FaceLoginProvider.cpp:21-51`（描述符表）、`FaceLoginCredential.cpp:486-524`（GetFieldState）、`:530-574`（GetStringValue）、`:583-589`、`:630-645`，加字段需同步 5 处 switch。定义具名常量。复评实测：除上述 5 个分支点外另有 8 个写侧调用点共 12 个裸字面量（`SetFieldString/SetFieldState(this, 1/3/4, ...)`，`FaceLoginCredential.cpp:465、821、1108、1139、1223-1226、1245-1248`），实际需同步 13 处；具名化为 C 级纯机械改动。

验收：`npm run build`（如有前端构建）与 enrollment UI 冒烟通过；删改一处表条目编译期报错。

### 8 — pipe_client 管道对测（B）

credential_provider 目前几乎零自动化覆盖。照 `tools/enrollment_support` 的本地命名管道 + 临时目录测法，覆盖 `credential_provider/pipe_client.cpp` 的超时、重连、回调时序（`ReadThreadProc` 约 145 行；回调先拷贝再调用的约定 `:310-318`；回调内自毁不能 join 自己 `:46-63`）。

验收：新增 tools/ 测试接入根 CMake `check` 目标；模拟对端异常断开/超时的用例通过。

### 9 — service_client 提取 request-ack 辅助函数（C）

`enrollment_app/service_client.cpp` 的 ReloadDatabase（`:32-122`）、QueryLearningStatus（`:128-185`）、ReloadConfig（`:187-241`）为三段约 50-90 行近似复制，仅消息常量/超时/响应判断不同，收成一个参数化函数。

### 10 — 相机后端接口化（A，条件触发）

`WebcamCaptureDS` 是具体类，`face_service/auth_worker.cpp:336-355` 用约 9 个 lambda 把具体方法接线到 SensorKnobs；换 Media Foundation 需在服务 worker 与注册程序两处宿主重接。抽 `ICameraCapture` 接口、DShow 作默认实现。SensorKnobs 已解决"调谐不依赖 DShow"，未解决"采集可替换"。

仅在确定要支持 Media Foundation 或多相机差异时实施；抽象本身有成本。

### 11 — app.js 模块化（C，优先级最低）

`enrollment_app/app.js` 1428 行单全局作用域、51 个顶层函数，靠横幅注释分 4 区段（复评实测；初评记 12 区段系误计）。WebView2 支持 ES modules，可拆为模块文件再由 CMake 合成链拼接。有 `tools/enrollment_ui/capture_flow_test.cjs` 与注释护航，痛点真实但不紧急。注意 `index.html.in` 含用户手工改动，勿覆盖。

### 12 — 同步原语不一致（C）

`face_service/authentication_residency.cpp:14、98-104` 使用 `std::mutex` + `std::condition_variable`，其余全部模块遵循 Win32 临界区 + 事件 + 原子变量的项目约定（AGENTS.md「编码与安全约束」）。收敛为 Win32 方式，或在 AGENTS.md 明确豁免该模块并注明原因。改动涉及线程生命周期，需过 `authentication_residency_test`。

### 13 — 修复 bump_version.ps1 断链（C）

AGENTS.md 在 865255f 重写后不再含 `**Windows 专属桌面应用**` 字样，`bump_version.ps1:72-74` 规则匹配 0 次，`-Dry` 实测在该规则处 throw；非 dry 模式下 CMakeLists/wails.json/main.go/README 已被逐文件先行写入（`Update-File` 无事务性），留下半同步状态。AGENTS.md 现已入库（git ls-files 确认），脚本尾注「AGENTS.md and info.json are gitignored」前半过时；BUILD.md:134「同步 CMake、Wails、README 和构建资源」与脚本实际 6 处目标不符。修复：更新或删除该规则，同步 BUILD.md 描述与尾注。

### 14 — DeleteFace use-after-erase（C）

`credential_store.cpp:489` erase 之后 490 行仍用 `rec.username.c_str()`：idx 为末元素时是 UB，否则把下一账户的用户名记进「Removed account」日志。照 `DeleteUserBySid:518-520` 先拷贝再 erase 的写法补齐；tools/credential_store 现有 8 个测试无一覆盖 DeleteFace，补用例（含删除末位账户）。

### 15 — enrollment_app 密码清零（C）

密码以 `std::wstring` 局部变量流经 COM 入口→校验→DPAPI 全链路（`WebviewHost.cpp:517、529-535、600`、`enrollment_accounts.cpp:148、334`、`session_identity.cpp:285、300`），目录内零处使用 `common/secure_clear.h` 的 `SecureWStringGuard/SecureClearWString`——face_service 与 credential_provider 均已使用，此处违反 AGENTS.md「所有退出路径都要清零」。用 RAII 守卫包住各局部点。

### 16 — app.js 长超时 COM 调用防冻结（C）

`H.SetConfig` 服务侧含 worker 替换握手，最长 25s（`service_client.cpp:197-202`）。app.js `onSaveSettings`（:382-406）与 `onCamPickerConfirm`（:1354-1378）同步调用，期间窗口冻结且可重复点击；`onSave`（:1100）与 `onAppendConfirm`（:1010）已有「setTimeout 延迟 + 禁用按钮」模式，套用即可。顺带可提 `restartPreview()` 辅助函数（「StartPreview + 三分支错误横幅」现存 6 份复制）。

### 17 — CP 引用计数接入（C）

`dllmain.cpp:18` 的 `g_cRefCount` 声明后从未被任何 AddRef/Release 增减，`DllCanUnloadNow:143` 恒视对象数为 0，「对象存活则 DLL 不可卸载」的 COM 契约失效（风险被 LogonUI 极少调用该接口掩盖）。最小修：class factory 与凭据对象的 AddRef/Release 接 DllAddRef/DllRelease。同区观察项：DLL_PROCESS_ATTACH 内做注册表读 + 目录创建 + 日志开文件（:97-112，loader lock 下非平凡 I/O，装机实测可用），改动时一并评估；SetUsageScenario 重入直接 `new` 覆盖旧凭据指针不 Release（`FaceLoginProvider.cpp:194-198`）。

### 18 — 拆分 PrepareFrames（B，#5 延伸）

`auth_frame_preparation.cpp:63` PrepareFrames 252 行，为全库现存最长函数；warmup / replay-settle / trigger / tune 四阶段天然是阶段函数。验收同 #5：单函数 ≤100 行，`AuthPipelineTest`/`LivenessTimingTest` 全绿，日志字面量不变。

### 19 — 复评低优先级杂项（C，按模块）

- face_service：`pipe_server.cpp:198-376` 三份 overlapped I/O 样板（约 3×40 行）可提泛型助手；`auth_pipeline.cpp:210-213、326-329` FaceRect 构造两处重复（#5 拆分引入），可提 `FaceRectFromDetection`；`auth_worker_client.cpp:91-198` Spawn 手动清理阶梯 5 处可 RAII，协议超时字面量（10000/1000/2000/5000/15000/200）未具名；`FaceService.cpp:794-868` `WorkerConsumed(true)` 重复 7 次可 scope guard；FaceService.cpp 996 行可按「认证会话 / 控制命令 / 安装」三块再拆。
- vision/storage：face_gain_tune.h 1185 行 header-only 被 6 处 include（改动触发全量重编译的可维护性单点），日志缓冲容量 96 重复 8+ 处；onnx_models.cpp 输入尺寸 112/512 未具名（112 约 15 处）；`config_util.cpp:145` 钳制字面量 0.70f/1.00f 应引用 `recognizer_profile.h` 的 kMin/kMaxMatchThreshold；`credential_store.cpp:190` version>=5 死分支（v4/v5 已提前拒绝）、`:444-445` old→new 日志两处均传新值、`credential_store.h:253` 默认参数 0.30f 为 dlib 时代陈迹；face_align 纯数学却无测试（补测成本最低收益明显）。
- enrollment_app：`EnrollmentWizard.cpp:342` label 拼 JSON 未转义（当前恒为固定三值，来源放开即成注入点）；`WebviewHost.cpp:510` 角度参数类型不符应返回 DISP_E_TYPEMISMATCH（现 BADPARAMCOUNT）；`EnrollmentWizard.h:157` rAF 注释陈旧（实为 setTimeout 33ms 轮询）。
- credential_provider：DllRegisterServer 忽略全部 RegSetValueExW 返回值（`dllmain.cpp:167-210`）；两处头文件注释漂移（状态机只列 4 态、字段表漏字段 4）；`FaceLoginCredential.h:134` m_pProvider 死成员；CMake 对 SHARED 库设 WIN32_EXECUTABLE 无效、netapi32/secur32 无对应符号。
- installer：`app.go:107-109、130-132、147-152` 三步失败不先 emit "fail" 事件（与其余步骤不一致，前端进度条停 running 态）；`com.go:84-159` 混放 COM 注册与文件系统 ACL 两职责（registry ACL 却在 security.go）。

## 完成记录

完成后在此按日期登记（日期、条目号、改动摘要、验证方式与结果），并同步更新上表状态。

### 2026-10-15 — #5（拆分 AuthPipeline::Run）

**`auth_pipeline.cpp`（395 行 → 编排 + 6 个阶段函数）**：`Run` 收为 92 行编排（守卫、截止时间、诊断与帧准备、状态上报、循环调用、结局分派）；循环体按阶段拆为私有成员 `CheckLoopGates`（每迭代取消/断开/窗口/空场门）、`RunLivenessLoop`（计帧主循环骨架，105 行/78 行纯代码）、`CorrectExposureBeforeFirstCount`（首计帧前三道曝光纠正：暗场恢复、中断调节确认、迟到纠正）、`AttemptBinding`（锚点/共识帧绑定尝试）、`PrefetchNextFrame`（重叠窗口预取）、`CountPadFrame`（PAD 判定与计帧）、`ResolveOutcome`（终态到用户消息映射），另加自由函数 `PositionDiscontinuous`（IoU 连续性门）。约 10 个布尔结局标志（failFastEmpty/failFastAttack/livenessInferenceError/identityRejected/identityRetryTimedOut…）合并为 `AuthLoopStop` enum，绑定与纠正的中间裁决分别为 `BindingDisposition`/`PreCountCorrection`，共享循环状态收进 `AuthLoopState`（含原 post-mortem 注释）。日志串新旧逐字比对一致（含一处差点引入的 "2s"→"2S" 笔误，已由比对拦回）。

**`auth_worker.cpp`（RunAuthenticationWorker 278 行 → 118 行主线 + 6 个自由函数）**：拆出 `PreloadWorker`（预载策略快照、模型加载、ORT 预热、相机骨架及计时日志）、`InitializeWorkerSession`（Hello→Init→配置解码→数据目录→日志握手）、`AwaitAuthCommand`（空闲命令等待，`CommandWait` enum 区分取消/无效/断链）、`WireAuthCallbacks` + `ExchangeBindingVerdict`（回调装配与单次绑定往返）、`OptOutOfPowerThrottling`（EcoQoS 退出，实测注释随迁）、`SendAuthTerminal`（终态回包）。顺带移除 `LoadModels` 从未使用的 `config` 参数（重编译暴露的 C4100，原代码即如此）。

- 验证：CTest `check` 11/11 通过（`AuthPipelineTest` 32 s 全场景含 tune-recovery/warmup-seed/dark-recovery/pacing/timeout/cancel，`LivenessTimingTest` 在列）；全目标 Release 构建零警告；两文件所有日志字面量与 HEAD 逐一 diff 一致。单函数行数：两文件最大为 `RunAuthenticationWorker` 118 行（含 ~30 行必要注释、86 行纯代码）与 `RunLivenessLoop` 105 行（78 行纯代码），其余 ≤92 行。
- 未做 GUI/锁屏实测，理由：纯结构重构，控制流逐路径等价（continue/break/return 语义、scoreFuture join 点、时序门与 pacing 逐行保留），worker 协议消息与顺序未变，自动化已覆盖循环全部结局分支。

### 2026-10-15 — #6、#9（第一梯队补充）

**#6 CMake MSVC runtime 样板收口**：根 `CMakeLists.txt` 在 `add_subdirectory` 之前统一 `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")`（附 CP 例外指引注释）；移除 6 个主模块与 12 个 tools 子目录共 21 处逐目标样板；`credential_provider/CMakeLists.txt` 保留 /MT 覆盖并补一行指向其 rationale 注释块。

- 验证：改动前后各工程 vcxproj 的 `RuntimeLibrary` 逐一比对一致（CP 为 MultiThreaded(D)，其余 MultiThreaded(D)DLL，含 build 目录历史残留的 ExposureReviewRepro/StaleReplayRepro 也不受影响——它们不在当前 CMake 源中）；Release 与 Debug 全目标构建通过；产物级复核 `FaceLoginService.exe` 导入 VCRUNTIME140/MSVCP140、CP DLL 无任何 CRT DLL 导入；CTest `check` 11/11 通过。

**#9 service_client 提取 request-ack 辅助函数**：新增 `RequestAckRoundTrip`（写请求 → 轮询等待 → 读完整消息模式消息 → 回 CONTROL_ACK，返回 `RequestAckOutcome{ok, response, error}`），`ReloadDatabase`/`QueryLearningStatus`/`ReloadConfig` 三段 50-90 行近似复制收拢为对它的调用，保留各自的消息常量、超时（5s/3s/25s）、轮询步长与结果解释。

- 顺带统一了三处此前不一致的细节：响应读取统一走 ERROR_MORE_DATA 循环与 4096 宽字符缓冲（原来只有 QueryLearningStatus 处理长消息，另两处单次 64 宽字符读取）；空响应统一记 ERROR_READ_FAULT 进入失败路径（原来 ReloadDatabase 会把成功读 0 字节误记为 GetLastError 的旧值）； PeekNamedPipe 失败统一按管道断开处理。差异均在原代码的边缘失败路径，正常往返语义不变。
- 验证：`EnrollmentSupportTest`（管道级对测，覆盖三种请求的接受/拒绝/对端断开/服务缺失及 CONTROL_ACK 断言）通过；CTest `check` 11/11 通过。属 IPC 边缘路径微调，未做 GUI 实测，理由：对端协议行为未变，测试已覆盖往返时序。

### 2026-10-15 — #1、#3、#4（第一梯队）

**#1 跨语言常量一致性校验**：新增 `installer/FaceLoginSetup/internal/cross_language_consistency_test.go`，含两个测试。

- `TestCredentialProviderCLSIDConsistent`：以 `credential_provider/resource.h` 的 DEFINE_GUID 为事实源构造规范 GUID 字符串，与 resource.h 注释、`dllmain.cpp` 的 `CLSID_STRING`、`FaceLoginProvider.cpp` 的注册表路径、安装器 `com.go` 的 `clsidCredentialProvider` 逐一比对。
- `TestInstallerConfigDefaultsMatchCPlusPlus`：调 `EnsureConfigDefaults` 实际写出 config.json，校验每个键都被 C++ `ConfigFromJson` 读取、每个值等于 C++ 侧默认（`kDefaultMatchThreshold` 从 `recognizer_profile.h` 解析）。
- 破坏性验证三个方向各一次：改 `com.go`、改 `config.go`、改 `resource.h` DEFINE_GUID 均使测试变红，恢复后复绿。
- 模型 hash 单源不在本次范围，仍以 [`todo.md`](todo.md)「模型 hash 跨语言单一来源」为准。

**#3 陈旧引用与缩进错位清理**：

- 17 处 `progressive-learning-v2.md` 引用统一改为 `docs/work/in-progress/progressive-learning-v2.md`（14 个文件）。
- `vision/onnx_models.h` 预处理注释 640×640 → 512×512，与 `onnx_models.cpp` 实现一致。
- `face_service/FaceService.cpp` RELOAD_DB 分支 498-516 行缩进左移 4 空格，纯空白改动。
- 提交前补查：4 处折行注释（enrollment_accounts.cpp、auth_pipeline.h、face_align.h、template_learner_test.cpp）在替换后产生 `docs/ docs/work/...` 重复前缀，已重排折行；另发现 `vision/liveness_types.h:11` 引用 `docs/threshold-calibration.md`，实际路径 `docs/work/completed/threshold-calibration.md`，一并修正。

**#4 魔法数字具名化**（所有常量值与原字面量一致，非行为改动）：

- `face_service/auth_pipeline.cpp`：新增 `kDiscardRetryPace`（30ms，附 5 秒预算约束注释），4 处 `sleep_for` 使用。
- `vision/onnx_models.cpp`：新增 `kMiniFasV2CropScale`（2.7f）/`kMiniFasV1SeCropScale`（4.0f）。
- `storage/credential_store.cpp`：新增 `kInitDistance`（1e10f）/`kNoFaceDistance`（1e9f）/`kAmbiguousRatioLimit`（0.75f），6 处使用。
- `vision/face_gain_tune.h`：新增 `kWhiteoutClipLuma`（245.0）/`kWhiteoutTileClipPercent`（70）/`kWhiteoutMinTiles`（20）；`clipped*10 >= count*7` 改写为等价的 `clipped*100 >= count*kWhiteoutTileClipPercent`。

**验证**：Release 全目标构建通过；CTest `check` 11/11 通过；`go test -count=1 ./...` 通过。改动均为注释/缩进/常量具名化（值不变），无行为变化，故未做 GUI 或锁屏实测。
