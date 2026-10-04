# FaceLogin 设计概览

FaceLogin 是面向 Windows 的人脸登录应用，通过 Credential Provider 接入登录与锁屏界面。本文只概述组件和安全边界；任务入口见 [`DEVELOPMENT.md`](../../DEVELOPMENT.md)，运行细节见 [`face-service.md`](../modules/face-service.md) 与 [`windows-clients.md`](../modules/windows-clients.md)，构建和运维分别见 [`BUILD.md`](../BUILD.md) 与 [`operations.md`](../operations/operations.md)。

## 组件

| 组件 | 主要职责 |
|---|---|
| `FaceLoginService.exe` | 持有用户数据和公共命名管道；锁屏时预载认证 worker。 |
| `FaceLoginService.exe -auth-worker` | 按请求打开 DirectShow 摄像头，运行检测、活体和特征提取；完成后退出。 |
| `FaceLoginCredentialProvider.dll` | 将认证结果交给 Windows 登录流程。 |
| `FaceLoginConsole.exe` | 管理注册信息和设置，通过命名管道通知服务重载。 |
| `FaceLoginSetup.exe` | 安装和卸载，不参与运行时认证。 |

核心实现使用 C++20、ONNX Runtime 和 Windows API；安装器使用 Go/Wails 与 Vue，注册界面使用 WebView2。认证模型、数据版本和阈值状态以 [`face-service.md`](../modules/face-service.md) 及 [`sface-trial.md`](../work/in-progress/sface-trial.md) 为准。

## 安全边界

- 所有组件间通信均为本机命名管道；服务端限制客户端为 SYSTEM 与 Administrators，并拒绝远程连接。具体消息格式见 [`IPC 契约`](../contracts/ipc.md) 和 [`worker IPC 契约`](../contracts/auth-worker-ipc.md)。
- `users.dat` 中的密码凭据由机器范围 DPAPI 保护；数据布局和兼容规则见 [`users.dat 契约`](../contracts/users-dat.md)。
- 活体检测必须通过；模型或认证错误按失败处理，仍可使用 Windows 密码登录。
- worker 独占模型和摄像头，单次认证后退出；服务父进程不在锁屏前打开摄像头。

## 账户兼容性

本地账户和 Microsoft 账户（MSA）支持注册与登录。MSA 身份优先通过 `GetUserNameExW(NameUserPrincipal)` 获取；该 API 不返回 UPN 时，Enrollment 使用 Windows IdentityStore 缓存回退。域账户目前没有正式验证，不承诺支持。

本地账户使用 `COMPUTERNAME\Username`，MSA 使用 UPN 交给 Windows 认证包。模型切换后的数据库兼容与重录要求见 [`users.dat 契约`](../contracts/users-dat.md) 和 [`SFace 试用说明`](../work/in-progress/sface-trial.md)。
