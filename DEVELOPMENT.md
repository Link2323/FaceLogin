# FaceLogin 代码导航

本文件只回答三件事：**改什么先看哪里、必须联查什么、最低怎么验证**。不要从头通读；先查任务路由，再按需打开一个模块文档或契约。

> 实现核对基线：2026-08 worker 隔离工作树。代码、构建结果和测试结果是事实源；文档冲突时，在同一任务中修正文档。安装包清单与构建命令以 [`docs/BUILD.md`](docs/BUILD.md) 为准。

## 使用方式

1. 从下面的任务路由选一行。
2. 打开“代码入口”和最多一个相关模块文档。
3. 涉及线格式/磁盘格式时再打开对应契约。
4. 修改后执行该行的最低验证。

能从代码直接搜索出的类声明、状态文本、JS 方法清单和源码行号不在此复制。

<a id="sec-task-routing"></a>
## 任务路由

| 改动目标 | 代码入口 | 联查文档 | 最低验证 |
|---|---|---|---|
| 认证循环、活体、识别、超时 | `face_service/FaceService.cpp`、`auth_pipeline.*`、`auth_worker*`、`onnx_models.*` | [`face-service.md`](docs/modules/face-service.md)、[`auth-worker-ipc.md`](docs/contracts/auth-worker-ipc.md) | Release 构建 + `AuthWorkerProtocolTest` + `ModelIntegrityTest` + `AuthWorkerLifecycleTest` + 锁屏 |
| worker 启动、私有 IPC、Job/超时 | `face_service/auth_worker_client.*`、`auth_worker_protocol.*`、`main.cpp` | [`auth-worker-ipc.md`](docs/contracts/auth-worker-ipc.md) | 两个 AuthWorker 测试 + 进程/Job 故障注入 |
| 摄像头/Session 0 | `face_service/webcam_capture_dshow.*`、`auth_worker.cpp` | [`face-service.md`](docs/modules/face-service.md) | `CameraLifecycleTest` child 100 轮 + 服务 Session 0 |
| IPC 字段、终态、缓冲区 | `common/ipc_protocol.*`、`face_service/pipe_server.*`、`credential_provider/pipe_client.*` | [`ipc.md`](docs/contracts/ipc.md) | 两端构建 + `IpcProtocolTest` + 断开/超长消息 |
| 人脸匹配、多人脸、`users.dat` | `face_service/credential_store.*`、`enrollment_app/EnrollmentWizard.*`、`credential_provider/FaceLoginProvider.cpp`（header） | [`users-dat.md`](docs/contracts/users-dat.md) | `CredentialStoreTest` + 保存/重载/匹配 |
| 锁屏磁贴、COM、LSA、卡死 | `credential_provider/FaceLoginCredential.*`、`credential_provider/auth_interaction_policy.h`、`credential_provider/FaceLoginProvider.*`、`credential_provider/pipe_client.*` | [`windows-clients.md`](docs/modules/windows-clients.md)；消息问题再读 IPC 契约 | Release 构建 + `CredentialProviderInteractionPolicyTest` + 锁屏登录/解锁 |
| 注册、人脸管理、WebView2 | `enrollment_app/EnrollmentWizard.*`、`enrollment_app/WebviewHost.*`、`enrollment_app/index.html` | [`windows-clients.md`](docs/modules/windows-clients.md)；落盘变化再读 users.dat 契约 | Release 构建 + 管理员 GUI |
| DataPath、日志、配置 | `common/data_path.*`、`face_service/FaceService.cpp`、`credential_provider/dllmain.cpp`、`credential_provider/FaceLoginProvider.cpp`、`enrollment_app/main.cpp`、`enrollment_app/EnrollmentWizard.cpp`、`installer/FaceLoginSetup/app.go`、`installer/FaceLoginSetup/internal/com.go`、`installer/FaceLoginSetup/internal/security.go` | 本文“路径与配置” | 三个 C++ 组件 + 安装器均检查 |
| 安装、卸载、ACL、部署资源 | `installer/FaceLoginSetup/app.go`、`installer/FaceLoginSetup/internal/`、`installer/FaceLoginSetup/resources/` | [`installer.md`](docs/modules/installer.md)、[`docs/BUILD.md`](docs/BUILD.md) | 在 Setup 目录 `go test ./...` + 资源白名单 |
| 安装器 Vue 前端 | `installer/FaceLoginSetup/frontend/src/` | [`installer.md`](docs/modules/installer.md) | 在 `frontend/` 运行 `npm run build` |
| 模型替换、量化、阈值 | `face_service/FaceService.cpp`、`installer/FaceLoginSetup/internal/extract.go`、`scripts/download_models.ps1`、`tools/threshold_calibration/` | [`face-service.md`](docs/modules/face-service.md)、[`threshold-calibration.md`](docs/threshold-calibration.md) | 标定 + hash/大小同步 + Go 测试 |

## 运行期架构

运行期边界以 [`AGENTS.md`](AGENTS.md) 为准：服务是命名管道服务端，Credential Provider 是 LogonUI 内 DLL，Enrollment 是独立 GUI；安装器非常驻且不参与 IPC。

## 关键文件索引

### `common/`

| 文件 | 作用 | 隐蔽约束 |
|---|---|---|
| `ipc_protocol.*` | 消息常量、认证结果解析/构造 | 改字段要同步所有读写端 |
| `data_path.*` | 服务端安全路径解析 | 白名单只被 face_service 使用 |
| `config_util.*` | `AppConfig` JSON + 注册表回退 | `CONFIG_RELOAD` 热加载 |
| `logger.*` | 每进程文件日志 + 环形缓冲 | 打开失败会静默丢弃文件日志 |
| `dpapi_util.*` | machine-scope DPAPI | 密文不可跨机恢复 |
| `secure_clear.h` | 敏感内存清零 | 所有密码退出路径都要使用 |
| `frame_image.h` | RGB 图像/裁剪/缩放 | 当前无 dlib |
| `sha256_util.*` | 模型完整性校验 | 换模型要同步多处 hash |

### 产品模块

- 服务生命周期、认证顺序、相机与阈值：[`docs/modules/face-service.md`](docs/modules/face-service.md)
- Credential Provider、Enrollment、WebView2：[`docs/modules/windows-clients.md`](docs/modules/windows-clients.md)
- 安装/卸载与 ACL 生命周期：[`docs/modules/installer.md`](docs/modules/installer.md)
- 安装包载荷白名单（唯一事实源）：[`docs/BUILD.md`](docs/BUILD.md)
- 命名管道线格式：[`docs/contracts/ipc.md`](docs/contracts/ipc.md)
- 父服务/认证 worker 私有线格式：[`docs/contracts/auth-worker-ipc.md`](docs/contracts/auth-worker-ipc.md)
- `users.dat` V4：[`docs/contracts/users-dat.md`](docs/contracts/users-dat.md)

<a id="sec-paths"></a>
## 路径与配置

默认安装器把 `InstallPath` 和 `DataPath` 都写为安装目录，通常是 `C:\Program Files\FaceLogin`：

```text
<DataPath>/
├── data/config.json
├── data/users.dat
├── log/service.log
├── log/auth_worker.log
├── log/credential_provider.log
├── log/enrollment.log
└── models/*.onnx
```

ProgramData 不是默认生产位置。

| 组件 | 路径行为 |
|---|---|
| Credential Provider | 直接读 DataPath；空值回退 `%ProgramData%\FaceLogin` |
| Enrollment | 直接读 DataPath；空值回退 `%ProgramData%\FaceLogin` |
| 服务（生产 EXE） | `ResolveSecureDataDir` 只信 EXE 目录；异常重定向 fail-closed |
| 服务（非生产 standalone） | DataPath → 空值时 ProgramData；候选必须在开发白名单 |
| standalone 初始化前日志 | 安全解析失败时仅日志临时回退 EXE 目录，再兜底 cwd；正式 Initialize 仍会拒绝不可信路径 |

改路径逻辑时必须同时检查三个组件和安装器注册表/ACL，不能只改 `common/data_path.cpp`。

配置字段事实源是 `common/config_util.h`;归一化逻辑在 `config_util.cpp`。认证阈值和实验依据只在服务模块与标定文档维护。

<a id="sec-models"></a>
## 模型与部署入口

模型已准备完成、普通开发不重新下载的规则只在 `AGENTS.md` 维护。模型角色、推理和阈值见 [`face-service.md`](docs/modules/face-service.md)。**安装载荷白名单只以 [`docs/BUILD.md`](docs/BUILD.md) 为准**；[`installer.md`](docs/modules/installer.md) 只解释安装生命周期和 `embed.FS` 风险。

## 文档地图

- 构建与打包：[`docs/BUILD.md`](docs/BUILD.md)
- 产品/安全设计：[`docs/design/overview.md`](docs/design/overview.md)
- 本地开发规范：[`docs/design/development.md`](docs/design/development.md)
- 运维与排障：[`docs/operations/operations.md`](docs/operations/operations.md)
- 认证 worker 迁移、性能与资源验收：[`docs/auth-worker-migration-completion.md`](docs/auth-worker-migration-completion.md)
- 迁移前性能优化实验 1–9：[`docs/performance-baseline.md`](docs/performance-baseline.md)
- 阈值标定：[`docs/threshold-calibration.md`](docs/threshold-calibration.md)
- 多角度设计：[`docs/side-face-plan-v2.md`](docs/side-face-plan-v2.md)
- 渐进学习：[`docs/progressive-learning.md`](docs/progressive-learning.md)
- 待办：[`docs/todo.md`](docs/todo.md)

## 文档维护规则

- 只在本索引记录跨模块关系和入口，不复制可搜索的实现清单。
- 线格式/磁盘格式只在 `docs/contracts/` 维护。
- 模块生命周期和设计约束只在 `docs/modules/` 维护。
- 路径、协议、模型、部署清单、CLSID、配置或 `users.dat` 变化时，同一任务同步受影响文档。
- 临时构建/下载产物任务结束即清理；正式 `build/` 目录除外。
