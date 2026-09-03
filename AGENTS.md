# FaceLogin — Agent Guide

本文件是 AI 编码 agent 在本仓库工作时的**守则**:只保留稳定约束、踩坑和代码里 grep 不到的约定。

**代码导航索引**:[`DEVELOPMENT.md`](DEVELOPMENT.md) 只提供任务路由、关键文件和验证入口;IPC / `users.dat` 契约在 [`docs/contracts/`](docs/contracts/),模块生命周期在 [`docs/modules/`](docs/modules/)。本文件不重复易变化的字段和清单。设计背景、运维、开发指南见 [`docs/design/`](docs/design/) 与 [`docs/operations/`](docs/operations/)。文档与实现冲突时,以代码、构建结果和测试结果为准,并在同一改动中修正文档;安装包资源清单以 [`docs/BUILD.md`](docs/BUILD.md) 为唯一事实源。

## Working Philosophy

以第一性原理！从原始需求和问题本质出发，不从惯例或模板出发。
1. 不要假设我清楚自己想要什么。动机或目标不清晰时，停下来讨论。
2. 目标清晰但路径不是最短的，直接告诉我并建议更好的办法。
3. 遇到问题追根因，不打补丁。每个决策都要能回答"为什么"。
4. 输出说重点，砍掉一切不改变决策的信息。

## 项目概述

FaceLogin 是 **Windows 专属桌面应用**(1.8.0-multi-angle),用人脸识别认证取代 Windows 锁屏密码提示,集成进 Windows Credential Provider 框架。四种语言:C++20(核心)、Go(安装器后端)、TypeScript/Vue 3(安装器前端)、内嵌 HTML/CSS/JS(注册 UI)。

## 目录结构

每个顶层目录对应一个产物或共享层,改代码前先定位目录:

```
face_service/        → FaceLoginService.exe (Windows 服务 + standalone 测试)
credential_provider/ → FaceLoginCredentialProvider.dll (LogonUI 加载的 COM DLL)
enrollment_app/      → FaceLoginConsole.exe + EnrollmentWizard (WebView2 注册 GUI)
common/              → facelogin_common.lib (服务/凭据提供器/注册 app 共享的静态库)
installer/FaceLoginSetup/ → FaceLoginSetup.exe (Go + Wails v2 安装器)
  resources/              → 安装器内嵌载荷镜像(按文件类型 gitignore,需手工同步)
scripts/             → 模型下载 / standalone 启动 / dev provider 卸载等辅助脚本
tools/               → 离线分析脚手架(embedding_test / pad_calibration / threshold_calibration)
assets/              → 图标 logo + 模型准备/转换工作区(正式模型会复制到安装器载荷镜像)
my_faces/            → 测试用真人脸图(不入包)
pad-calibration/     → PAD 标定数据集 CSV(不入包)
docs/                → BUILD / 性能 / 阈值标定等(见文末索引)
build/               → CMake 输出(Release 下的 5 个运行时 DLL 随安装包发布)
```

> 注:注册 app 目录是 `enrollment_app/`,不是 `console/`。

## 构建与运行

完整构建命令、前置依赖、安装包重编流程、编译产物路径见 [`docs/BUILD.md`](docs/BUILD.md)。两条一键命令(2026-09-02 起代替手工链,均已端到端验证):`scripts/build_installer.ps1` 一条命令全链出安装包(白名单清理 + no-op/`-clean` 瞬时锁自动防护,~1 分钟);`scripts/bump_version.ps1 <x.y.z[-suffix]>` 版本号 6 处同步(含 AGENTS.md 与 info.json 两处仅磁盘,`-Dry` 可预览)。几条关键不变量必须记住:

- `installer/FaceLoginSetup/resources/` 中的二进制与模型按文件类型被 gitignore,且除注册 app/模型的构建期复制外,CMake 不会自动同步;`build_installer.ps1` 负责按白名单拷贝+清理,手工打包时仍必须按 `docs/BUILD.md` 核对内容。
- 正好 **5 个运行时 DLL** 随安装包发布(onnxruntime 的传递闭包,从 `build/face_service/Release/` 拷贝)。
- `FaceLoginCredentialProvider.dll` 使用静态 CRT (/MT),不需要其中任何一个 DLL。
- `PadCalibration.exe` / `EmbeddingTest.exe` 是 `tools/` 下的离线开发工具,不属于安装载荷;`embed.FS` 会嵌入资源目录中的所有文件,打包前要清除意外残留。
- **VS 2026(MSVC 19.5x)构建已启用 `/MP` 文件级并行** —— 配合 `TrackFileAccess=false` + `ErrorReporting=None` 避免 cl.exe 卡死(2026-08-13 重验证:连续 clean rebuild 稳定,全量构建 39s → 8s)。加 `--parallel` 可再获项目级并行(→ 7.8s)。详见 `docs/BUILD.md`。
- 不编辑 `installer/FaceLoginSetup/frontend/wailsjs/` 生成绑定;由 `wails build` / `wails dev` 重新生成。

### standalone 与开发测试

项目仍无 CI/CD；需要真实摄像头/锁屏的路径走 standalone 或安装服务实测:

```
.\scripts\start_standalone.bat       # 从仓库根目录运行;前台启动,Ctrl+C 退出
```

也可从仓库根直接运行 `.\build\face_service\Release\FaceLoginService.exe -standalone`;二进制其余公开参数为 `-install` / `-uninstall`，私有 `-auth-worker` 只能由父服务启动。开发期高频辅助脚本还包括 `scripts/unregister_dev.bat`。协议、凭据和 worker 生命周期已有 `AuthWorkerProtocolTest`、`CredentialStoreTest`、`AuthWorkerLifecycleTest`；相机资源用 `CameraLifecycleTest --mode inproc|child`。完整命令见 `docs/BUILD.md`。`download_models.ps1` / `download_minifas_models.ps1` 不是常规构建步骤,只在新克隆缺模型、文件校验失败或明确升级模型时使用。静态检查:MSVC `/W4`(`/WX-`)+ 前端 `vue-tsc --noEmit`;安装器仍须在 `installer/FaceLoginSetup/` 运行 `go test ./...` 校验嵌入资源。

### 模型文件

v1.6 共 4 个正式 ONNX 模型,当前工作区已准备完成。普通构建/打包直接复用 `assets/models/` 与 `installer/FaceLoginSetup/resources/models/` 中已有文件,**不要重复下载或重新量化**。仅在新克隆缺模型、SHA-256 校验失败或任务明确要求升级模型时,才运行 `powershell -File scripts\download_models.ps1`;构建注册 app 时会把同 SHA-256 的正式模型复制到安装器资源镜像。两处不是独立来源:`assets/models/` 是准备/转换工作区,安装器目录是待打包镜像。用途与部署约束见 [DEVELOPMENT.md 模型索引](DEVELOPMENT.md#sec-models),推理细节见 [`docs/modules/face-service.md`](docs/modules/face-service.md):

| 文件 | 大小 | 角色 |
|---|---|---|
| `det_10g_gnkps.onnx` | ~4.3 MB | SCRFD 检测 + 5 关键点(INT8,输入 512×512) |
| `w600k_r50.onnx` | ~44 MB | InsightFace IResNet-50,512 维嵌入(INT8 QDQ) |
| `MiniFASNetV2.onnx` | ~1.7 MB | 反欺诈(2.7× 裁剪) |
| `MiniFASNetV1SE.onnx` | ~1.7 MB | 反欺诈(4.0× 裁剪) |

**dlib 已移除。** vcpkg manifest 仅依赖 onnxruntime;`common/frame_image.h` 复刻原 dlib 流水线,PAD 分数与 0.281 阈值不变;SCRFD 5 关键点驱动对齐(`face_service/face_align.h`)。

## 架构

### 运行期组件与部署产物

运行期有三个宿主进程:`LogonUI.exe`(加载 Credential Provider DLL)、`FaceLoginService.exe`、`FaceLoginConsole.exe`。Credential Provider 和注册控制台分别通过 Windows 命名管道与服务通信(管道名 `\\.\pipe\FaceLoginPipe`,UTF-16LE,服务模式 DACL = SYSTEM + Administrators,`PIPE_REJECT_REMOTE_CLIENTS`):

```
[锁屏]                                 [注册控制台]
LogonUI.exe                            FaceLoginConsole.exe
    | (COM)                                | (WebView2 + DirectShow 相机)
    v                                      v
FaceLoginCredentialProvider.dll         EnrollmentWizard
    | (管道: AUTH_REQUEST,                 | (管道: RELOAD_DB,
    |  STATUS, AUTH_SUCCESS, AUTH_ACK)      |  CONFIG_RELOAD, CONTROL_ACK;日志直接读文件)
    v                                      v
FaceLoginService.exe（常驻父进程） <----> 凭据存储 (users.dat V4)
    | (私有继承管道；仅 embedding/状态)       | (DPAPI 加密)
    v
FaceLoginService.exe -auth-worker（一次认证即退出） config.json (热重载)
    | (Session 0；DirectShow)
    v
ONNX 模型 (SCRFD gnkps + r50 + 双 MiniFAS)
```

- **FaceLoginService.exe** —— Windows 服务(自动启动)。常驻父进程仅持有公开管道、`users.dat` 与密码解密；锁屏预加载的 `-auth-worker` 子进程持有 ONNX，认证请求后才打开 DirectShow 相机，完成一次认证即退出。禁止锁屏阶段预开摄像头。standalone 同样使用 DirectShow。
- **FaceLoginCredentialProvider.dll** —— LogonUI 加载的 COM DLL,实现 `ICredentialProvider`/`ICredentialProviderCredential`。
- **FaceLoginConsole.exe** —— WebView2 注册 GUI。`EnrollmentWizard` 经 COM `IDispatch` 暴露 **35 个方法**(dispId 1–35)。支持多角度采集。必须管理员运行。
- **FaceLoginSetup.exe** —— 非常驻部署工具,不参与运行期管道通信。Go Wails v2 安装器通过 `embed.FS` 内嵌全部部署资源;安装/卸载流程见 [`docs/modules/installer.md`](docs/modules/installer.md),不要在本文件维护步骤数。

### 通用库(`common/`)

静态库 `facelogin_common`,被三进程共享:

- **Logger** —— 线程安全单例,按天轮转,2000 行环形缓冲(UI 日志页四来源均读当日日志文件,console 来源读 `console.log`;环形缓冲仅作进程内诊断保留);宏 `FACELOGIN_DEBUG/INFO/WARN/ERROR`。**日志/数据默认在 Program Files 安装目录下,不在 ProgramData** —— 安装器把 `DataPath` 写成安装目录本身;ProgramData 仅是回退。三组件的路径解析逻辑不一致(face_service 有白名单,另两个直接信任 DataPath),改路径逻辑三个都要看。详见 [DEVELOPMENT.md 路径索引](DEVELOPMENT.md#sec-paths)。
- **IPC 协议** —— Credential Provider 公共命名管道见 [`docs/contracts/ipc.md`](docs/contracts/ipc.md)；父服务/一次性 worker 私有继承管道见 [`docs/contracts/auth-worker-ipc.md`](docs/contracts/auth-worker-ipc.md)。
- **DPAPI** —— `DpapiUtil::Protect/Unprotect`(`CRYPTPROTECT_LOCAL_MACHINE`)。
- **ConfigUtil** —— `AppConfig`,经 `CONFIG_RELOAD` 热重载。字段速查见 [DEVELOPMENT.md 路径与配置](DEVELOPMENT.md#sec-paths)。
- **RegistryUtil** —— `HKLM\SOFTWARE\FaceLogin`(DataPath)辅助。

### 人脸识别流水线(认证核心)

`FaceService::ProcessAuthRequest` 的认证循环要点:

1. 延迟相机初始化;自适应曝光预热(`face_service/exposure_warmup.h`)——按 DS 帧序号去重采样均值亮度,2 帧窗口 max-min ≤ 20 即通过:稳定场景 2 个不同帧开门(严于旧固定 3 次循环的实际效果),慢收敛场景最多 10 帧。
2. **融合一致性+活体循环(全部计帧必须通过)**:每帧 SCRFD 检测 → 双 MiniFAS 活体(50/50 融合),单预取槽软件流水线调度——PAD(N) 异步运行时主线程预取帧 N+1 的抓帧+检测,判定逻辑与逐帧顺序不变。帧 pacing 由显式抓帧守卫保证:相邻已计帧采集间隔 ≥60ms 且帧序号严格递增(锚点在预取前乐观前移)。身份绑定在部分绑定帧(首锚点锁定 SID,后续绑定帧必须返回相同 SID,否则 fail closed 换脸攻击);非绑定帧用 bbox IoU 连续性。
3. 匹配:欧氏距离 < 阈值(`match_threshold`,可配置)**且** 最佳/次佳比门控。具体阈值与钳制区间见代码及 `docs/threshold-calibration.md`。
4. 全局超时(PAD 另有独立时间窗口)→ `AUTH_TIMEOUT`。

**活体检测(强制,fail-closed)** —— 双 MiniFAS 50/50 融合,阈值 `anti_spoof_threshold`(标定值见 `docs/threshold-calibration.md`)。无活体模型服务拒绝启动;任何模型错误返回关闭判定；活体检测不是可配置功能。

**性能** —— 慢机活体循环双峰的根因已定位并修复(worker 在 AUTH_START 解除 EcoQoS 电源限流,commit `40d018e`;历史"CPU 频率是主要决定变量"的结论即源于此,频率探针证据见 baseline §14)。相机枚举+图骨架已移入锁屏预载期(`0709653`),设备激活仍只在 AUTH_START 后。现行基线:开发机 E2E ~800ms、慢机 ~950–1090ms。除非有新的可复现实验数据,不要做逐机参数调优。迁移前的性能实验、嵌入次数与被否决方案见 [`docs/performance-baseline.md`](docs/performance-baseline.md)；worker 架构、正式 A/B 和资源验收见 [`docs/auth-worker-migration-completion.md`](docs/auth-worker-migration-completion.md)。服务强制校验 recognizer SHA-256(值见代码)。

### 凭据存储 V4

`users.dat` 只接受和写入 V4 多人脸格式；安装此版本前必须完整卸载旧版本并重新录入。当前 embedding 统一 512 维；dlib 已彻底移除,**不要去找 dlib 残留代码**。多角度每角度独立存储,绝不平均。完整字节布局见 [`docs/contracts/users-dat.md`](docs/contracts/users-dat.md)。

### 关键标识符

- **CLSID**:`{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}`,在 4 处以字面量出现(`credential_provider/resource.h`、`credential_provider/dllmain.cpp`、`credential_provider/FaceLoginProvider.cpp`、`installer/FaceLoginSetup/internal/com.go`)—— **改 CLSID 必须四处同步**。
- 公共与私有 worker 管道格式分别见 [`docs/contracts/ipc.md`](docs/contracts/ipc.md) / [`auth-worker-ipc.md`](docs/contracts/auth-worker-ipc.md);CLSID 同步位置见 [`docs/modules/windows-clients.md`](docs/modules/windows-clients.md);DataPath 入口见 [DEVELOPMENT.md 路径索引](DEVELOPMENT.md#sec-paths)。

## 重要模式与约束

这是本文件的核心价值 —— 下面这些是 agent 最容易踩坑的地方:

- **仅 Windows、仅 x64。** 大量 Win32 API(COM、DPAPI、SCM、命名管道、DirectShow)。
- **无网络 API。** 所有 IPC 都是本地命名管道;从不监听 TCP 套接字。
- **C++20 配 `/utf-8`。** 源文件含原始非 ASCII 字符(—、→);MSVC 必须按 UTF-8 解析,否则 GBK/936 代码页下宽字符字面量乱码。
- **线程安全:** `CRITICAL_SECTION` 同步,`std::atomic<bool>` 服务运行标志,`HANDLE` 事件跨线程信号。注册 app 帧缓存由 `EnrollmentWizard::m_frameCacheMutex` 保护;不用条件变量。
- **错误处理:** 函数返回 `bool`;详细错误经 `FACELOGIN_ERROR` 记录。不用异常做控制流。
- **凭据内存安全:** 密码存于 `std::wstring`,每个退出路径 `SecureZeroMemory`。绝不记录密码内容。
- **凭据绑定本机:** DPAPI `CRYPTPROTECT_LOCAL_MACHINE`,凭据不可跨机迁移或重装系统后恢复。
- **COM 套间:** 凭据提供器与 WebView2 注册 UI = Apartment 线程；服务 worker = `COINIT_MULTITHREADED`。DirectShow 必须复用当前线程已初始化的 COM 公寓，且只能在自身初始化的同一线程调用 `CoUninitialize`。
- **MSA 支持:** UPN 经 `GetUserNameExW` 解析,回退到 IdentityStore 注册表缓存(`docs/design/overview.md` §4.2)。检测并阻止无密码 MSA 账户。
- **安装器是 Go,不是 C++。** 安装/卸载流程改动放 `installer/FaceLoginSetup/internal/`([安装器模块](docs/modules/installer.md))。
- **多角度注册不平均:** 每角度一条人脸记录,跨角度绝不平均嵌入。
- **临时产物即时清理:** 验证/测试产生的临时目录、临时构建输出、下载的中间文件(如 `build-test/`、`*.nupkg`、`webview2-pkg/` 等一次性产物)在任务结束时立即删除,不要留在仓库里。正式的 `build/` 例外(已 gitignore,是开发产物)。判据:这个产物下次还会用吗?不会就删。

## 完成标准

- 先按 [DEVELOPMENT.md 任务路由](DEVELOPMENT.md#sec-task-routing) 定位入口和关联契约,不要靠目录名猜实现。
- 代码改动至少通过对应模块的构建/静态检查;安装器改动必须在 `installer/FaceLoginSetup/` 运行 `go test ./...`,前端改动必须在其 `frontend/` 下运行 `npm run build`。
- 认证、相机、Credential Provider 等缺少自动化覆盖的路径,交付时说明实际执行的 standalone/GUI/锁屏验证;未执行也要明确说明原因。
- 若改动改变路径、协议、模型、部署清单、CLSID、配置字段或 `users.dat` 格式,同一任务必须同步 `AGENTS.md` / `DEVELOPMENT.md` / 对应 `docs/` 中受影响的事实。

## 相关文档

修改对应模块前先查阅:

- [`DEVELOPMENT.md`](DEVELOPMENT.md) —— **精简代码导航**(任务 → 入口 → 联查文档 → 验证)
- [`docs/contracts/ipc.md`](docs/contracts/ipc.md) / [`auth-worker-ipc.md`](docs/contracts/auth-worker-ipc.md) / [`users-dat.md`](docs/contracts/users-dat.md) —— 公共线格式、私有 worker 线格式与磁盘格式契约
- [`docs/modules/face-service.md`](docs/modules/face-service.md) / [`windows-clients.md`](docs/modules/windows-clients.md) / [`installer.md`](docs/modules/installer.md) —— 按模块的生命周期与修改边界
- [`docs/design/overview.md`](docs/design/overview.md) —— 项目简介、技术栈、安全设计、账户兼容性
- [`docs/design/development.md`](docs/design/development.md) —— 本地开发流程、编码规范
- [`docs/operations/operations.md`](docs/operations/operations.md) —— 系统要求、故障排查
- [`docs/BUILD.md`](docs/BUILD.md) —— 构建/重编/打包流程
- [`docs/auth-worker-migration-completion.md`](docs/auth-worker-migration-completion.md) —— worker 迁移、性能、资源与发布验收
- [`docs/performance-baseline.md`](docs/performance-baseline.md) —— 迁移前性能实验 1–9、低性能机器与模型优化历史
- [`docs/threshold-calibration.md`](docs/threshold-calibration.md) —— 阈值标定方法
- [`docs/side-face-plan-v2.md`](docs/side-face-plan-v2.md) / [`progressive-learning.md`](docs/progressive-learning.md) —— 多角度与渐进式学习设计;渐进式学习**可实施方案**(吸收学术经验,分阶段+标定门控)见 [`progressive-learning-v2.md`](docs/progressive-learning-v2.md)
- [`docs/todo.md`](docs/todo.md) —— 待办
