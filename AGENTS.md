# FaceLogin — Agent Guide

本文件只记录跨模块的稳定约束和不易从代码中看出的注意事项。任务入口与验证命令见 [`DEVELOPMENT.md`](DEVELOPMENT.md)；IPC、存储格式、模块生命周期和构建资源清单分别以 [`docs/contracts/`](docs/contracts/)、[`docs/modules/`](docs/modules/) 和 [`docs/BUILD.md`](docs/BUILD.md) 为准。文档与实现冲突时，以代码和实际构建、验证结果为准；修正本次改动涉及的文档事实，并简要记录其他发现的不一致。

## 工作原则

1. 从需求和问题本身出发。只有当歧义会影响目标、验收或不可逆操作时才提问；可逆的实现细节自行判断。
2. 目标明确但路径不是最短时，指出更直接的做法。
3. 查明足以解释并修复问题的原因，优先做最小完整修复；扩大范围要有直接证据。
4. 汇报结论和影响决策的信息。

## Git 提交

提交信息统一使用 `type(scope): 描述` 格式。

## 项目与目录

FaceLogin 是 Windows x64 桌面应用，核心为 C++20，安装器使用 Go/Wails，安装器前端使用 TypeScript/Vue，注册界面使用内嵌 HTML/CSS/JavaScript。

| 目录 | 用途 |
|---|---|
| `face_service/` | Windows 服务与 standalone 服务程序 |
| `credential_provider/` | 由 LogonUI 加载的 Credential Provider COM DLL |
| `enrollment_app/` | WebView2 注册 GUI（不是 `console/`） |
| `common/` | 服务、凭据提供器和注册程序共享库 |
| `installer/FaceLoginSetup/` | Go/Wails 安装器；`resources/` 是嵌入载荷镜像 |
| `assets/`、`scripts/` | 图像与模型资源、模型和开发辅助脚本 |
| `tools/` | 离线分析与标定工具 |
| `docs/` | 构建、契约、模块、设计与运维文档 |
| `build/` | CMake 构建输出 |

## 构建、资源与模型

- 构建、依赖和打包流程以 [`docs/BUILD.md`](docs/BUILD.md) 为准。优先使用 `scripts/build_installer.ps1`；版本同步使用 `scripts/bump_version.ps1`。
- `installer/FaceLoginSetup/resources/` 中的二进制和模型按文件类型被 Git 忽略。打包脚本按白名单复制并清理资源；手工打包前按 `docs/BUILD.md` 核对。安装包需要的 5 个运行时 DLL 来自 `build/face_service/Release/`；Credential Provider 使用静态 CRT，不依赖这些 DLL。
- `embed.FS` 会嵌入资源目录中的全部文件，打包前清除意外残留。`tools/` 中的离线工具不属于安装载荷。
- 不编辑 `installer/FaceLoginSetup/frontend/wailsjs/` 下的生成绑定；通过 Wails 命令重新生成。
- 普通构建和打包复用现有模型文件，不重复下载或量化。只有新克隆缺模型、哈希校验失败或任务明确要求升级时，才运行模型下载脚本。模型来源、哈希和转换规则见 `third_party/models/` 与 [`DEVELOPMENT.md`](DEVELOPMENT.md#sec-models)。
- 依赖或模型变更时，同步更新适用的许可、来源记录和随包声明；事实源及更新脚本见 `docs/BUILD.md`、`third_party/sources.json`。

## 架构边界与易踩坑

- 运行期由 LogonUI、`FaceLoginService.exe` 和 `FaceLoginConsole.exe` 协作。Credential Provider 与注册程序通过本地命名管道和服务通信；服务与认证 worker 使用私有继承管道。协议细节见 [`docs/contracts/ipc.md`](docs/contracts/ipc.md) 和 [`docs/contracts/auth-worker-ipc.md`](docs/contracts/auth-worker-ipc.md)。项目不使用网络 API，也不监听 TCP 端口。
- 服务父进程管理公共管道和凭据存储；认证 worker 持有推理模型。锁屏预载阶段禁止打开摄像头，认证开始后才激活设备。配置了 `DevicePath` 时必须使用指定设备；找不到设备就失败，不得静默切换到其他摄像头。生命周期细节见 [`docs/modules/face-service.md`](docs/modules/face-service.md)。
- `DataPath` 默认指向安装目录，ProgramData 仅作回退。服务、Credential Provider 和注册程序的路径校验逻辑不同；修改路径处理时要检查三个组件。入口见 [`DEVELOPMENT.md`](DEVELOPMENT.md#sec-paths)。
- 服务 worker 使用 MTA；Credential Provider 和 WebView2 注册界面使用 Apartment 线程。DirectShow 必须复用当前线程已初始化的 COM 公寓；只有在同一线程初始化 COM 的代码才能调用 `CoUninitialize`。
- 公共管道、worker 管道和 `users.dat` 格式以 `docs/contracts/` 中的契约文档为准。不要在本文件复制字段表或字节布局。
- `users.dat` 当前格式和模型标识见 [`docs/contracts/users-dat.md`](docs/contracts/users-dat.md)。格式、识别模型或模型标识变更可能影响现有录入，实施前先核对契约和迁移行为；多角度记录分别保存，不跨角度平均嵌入。
- Credential Provider CLSID 同时出现在 `credential_provider/resource.h`、`credential_provider/dllmain.cpp`、`credential_provider/FaceLoginProvider.cpp` 和 `installer/FaceLoginSetup/internal/com.go`；改动时四处同步。

## 编码与安全约束

- 仅支持 Windows x64。C++ 使用 `/utf-8`；不要依赖本机默认代码页。
- 按现有同步方式保护共享状态：Win32 临界区、原子变量和事件。注册程序的帧缓存由 `EnrollmentWizard::m_frameCacheMutex` 保护；该模块不用条件变量。
- 函数以 `bool` 返回成功状态，详细错误通过 `FACELOGIN_ERROR` 记录；不用异常做控制流。
- 密码保存在 `std::wstring` 中，所有退出路径都要清零；日志绝不记录密码。DPAPI 使用 `CRYPTPROTECT_LOCAL_MACHINE`，凭据绑定本机。
- 处理 MSA 账户时，UPN 解析和 IdentityStore 缓存回退行为见 [`docs/design/overview.md`](docs/design/overview.md)。无密码 MSA 账户必须被检测并阻止。
- 安装器是 Go 项目，安装与卸载逻辑放在 `installer/FaceLoginSetup/internal/`，参见 [`docs/modules/installer.md`](docs/modules/installer.md)。
- 一次性验证和下载中间产物在任务结束时清理；正式 `build/` 输出保留。

## 验证与文档同步

- 代码改动至少运行对应模块的构建或静态检查。安装器改动运行 `go test ./...`；前端改动运行 `npm run build`，命令目录和其他验证入口见 [`docs/BUILD.md`](docs/BUILD.md)。
- 认证、相机和 Credential Provider 路径若未由自动化覆盖，交付时说明是否执行 standalone、GUI 或锁屏实测；未执行时说明原因。
- 若改动影响路径、协议、模型、部署资源、CLSID、配置或 `users.dat` 格式，同步更新 `AGENTS.md`、`DEVELOPMENT.md` 及相关契约/模块文档中的事实。

## 参考文档

- [`DEVELOPMENT.md`](DEVELOPMENT.md)：任务路由、入口文件和验证命令。
- [`docs/contracts/`](docs/contracts/)：公共 IPC、worker IPC 与 `users.dat` 契约。
- [`docs/modules/`](docs/modules/)：服务、Windows 客户端与安装器生命周期和边界。
- [`docs/BUILD.md`](docs/BUILD.md)：构建、打包、资源与许可声明。
- [`docs/design/overview.md`](docs/design/overview.md)、[`docs/design/development.md`](docs/design/development.md)：架构、安全与开发约定。
- [`docs/operations/operations.md`](docs/operations/operations.md)：系统要求与故障排查。
- [`docs/work/README.md`](docs/work/README.md)：进行中与已完成的阶段性工作记录。
