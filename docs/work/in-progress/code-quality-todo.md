# 代码质量评估与改善待办

2026-10-15 对全库做了一次可读性 / 可维护性 / 可扩展性评估，覆盖 C++ 核心（face_service、common、vision、storage）、Credential Provider、注册界面（enrollment_app）、安装器（Go）、构建系统、文档与测试体系，证据定位到文件与行号。本文件收纳由评估产生的改善建议并登记完成情况；与 [`todo.md`](todo.md) 重叠的项只跟踪状态，详细描述以该处为准。全部清零后本文件移入 `../completed/`。

## 评估结论（2026-10-15）

| 维度 | 得分 | 主要依据 |
|---|---|---|
| 可读性 | 7.5 | 命名与"为什么"注释是标杆级（带日期的实测记录）；扣分在约 10 个超百行函数（最长 `AuthPipeline::Run` 约 395 行）、散落魔法数字、一处缩进错位、app.js 单作用域 1428 行。 |
| 可维护性 | 7 | 依赖方向单向、bool+FACELOGIN_ERROR 约定执行一致、IPC 协议单源、文档逐项对得上；扣分在 CLSID 四处 / DataPath 五处 / 模型哈希双份等靠文档约定同步的重复，credential_provider 近零自动化覆盖。 |
| 可扩展性 | 6 | AuthPipeline / TemplateLearner / AuthenticationResidency 三个接缝与模型版本化设计优秀；扣分在相机后端无抽象、凭据字段 ID 魔法数散布 5 处 switch、dispId 双手工表、新增宿主方法需 4 处同步。 |
| 综合 | 7 | 文档（9）、安装器（8.5）、构建（8）、C++ 核心（7~7.5）、credential_provider（6~8）、enrollment_app（5~7）、测试（7）。 |

整体判断：工程素养部分（文档、注释、安全纪律、测试策略）是强项；短板集中在"多份事实源"与"无抽象的可替换点"。第一梯队完成后可维护性预计可到 8；相机抽象与 dispId 单表化落地后可扩展性有望到 7.5。

## 改善清单

难度标记沿用 [`todo.md`](todo.md)：**C** 局部小改动，**B** 单模块改造并补测试，**A** 跨模块/跨语言契约改动，**S** 多线程/COM/IO 生命周期重构。状态：未开始 / 进行中 / 已完成。

| # | 难度 | 建议 | 涉及位置 | 状态 |
|---|---|---|---|---|
| 1 | C | 跨语言常量一致性校验（CLSID、配置默认值） | `installer/FaceLoginSetup/` | 已完成（2026-10-15） |
| 2 | A | DataPath 解析统一 —— 已在 [`todo.md`](todo.md)「DataPath 解析统一」登记，以该处为准 | `common/data_path.cpp` 等 | 未开始 |
| 3 | C | 陈旧引用与缩进错位清理（onnx_models.h 注释部分已登记于 [`todo.md`](todo.md)「清理推理接口与离线标定的旧说明」） | 见下文 | 已完成（2026-10-15） |
| 4 | C | 魔法数字具名化 | 见下文 | 已完成（2026-10-15） |
| 5 | B | 拆分 `AuthPipeline::Run` | `face_service/auth_pipeline.cpp:45-439` | 未开始 |
| 6 | C | CMake MSVC runtime 样板收口 | 根及各模块 `CMakeLists.txt` | 未开始 |
| 7 | B | dispId 单表化与凭据字段 ID 具名 | `enrollment_app/WebviewHost.cpp`、`credential_provider/` | 未开始 |
| 8 | B | pipe_client 管道对测 | `credential_provider/pipe_client.cpp` | 未开始 |
| 9 | C | service_client 提取 request-ack 辅助函数 | `enrollment_app/service_client.cpp` | 未开始 |
| 10 | A | 相机后端接口化（仅在确需 Media Foundation 或多相机差异时实施） | `vision/`、`face_service/auth_worker.cpp:336-355` | 未开始 |
| 11 | C | app.js 模块化（优先级最低） | `enrollment_app/app.js` | 未开始 |
| 12 | C | 同步原语不一致：收敛或在文档豁免 | `face_service/authentication_residency.cpp` | 未开始 |

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
- credential_provider 凭据字段 ID 0-4 为魔法数，散布 `FaceLoginProvider.cpp:21-51`（描述符表）、`FaceLoginCredential.cpp:486-524`（GetFieldState）、`:530-574`（GetStringValue）、`:583-589`、`:630-645`，加字段需同步 5 处 switch。定义具名常量。

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

`enrollment_app/app.js` 1428 行单全局作用域、约 50 个顶层函数，靠横幅注释分 12 区段。WebView2 支持 ES modules，可拆为模块文件再由 CMake 合成链拼接。有 `tools/enrollment_ui/capture_flow_test.cjs` 与注释护航，痛点真实但不紧急。注意 `index.html.in` 含用户手工改动，勿覆盖。

### 12 — 同步原语不一致（C）

`face_service/authentication_residency.cpp:14、98-104` 使用 `std::mutex` + `std::condition_variable`，其余全部模块遵循 Win32 临界区 + 事件 + 原子变量的项目约定（AGENTS.md「编码与安全约束」）。收敛为 Win32 方式，或在 AGENTS.md 明确豁免该模块并注明原因。改动涉及线程生命周期，需过 `authentication_residency_test`。

## 完成记录

完成后在此按日期登记（日期、条目号、改动摘要、验证方式与结果），并同步更新上表状态。

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
