# FaceLogin 运维手册

本文件覆盖**运行环境要求**与**故障排查**,与 [`docs/BUILD.md`](../BUILD.md)(怎么构建)互补。任务/文件入口见 [`DEVELOPMENT.md`](../../DEVELOPMENT.md),协议契约见 [`docs/contracts/`](../contracts/)。

---

## 一、系统要求

- Windows 10 21H2+ / Windows 11
- x64 处理器
- USB 摄像头（或内置摄像头），支持 1280×720 分辨率
- 管理员权限（用于安装、注册COM组件、服务管理）
- WebView2 运行时（Windows 11 内置，Windows 10 自动安装）

---

## 二、故障处理

### 2.1 日志文件

默认安装下，所有日志位于**安装目录**下 `%ProgramFiles%\FaceLogin\log\`（即注册表 `HKLM\SOFTWARE\FaceLogin\DataPath` 指向的目录，安装器默认把 DataPath 设为安装目录本身）：

| 日志文件 | 来源 |
|---|---|
| `service.log` | 人脸识别服务 |
| `auth_worker.log` | 一次性认证子进程（模型、相机、资源回收） |
| `credential_provider.log` | 登录界面组件 |
| `console.log` | 注册控制台 |

> 日志文件为 **UTF-8（无 BOM）**，记事本、`grep`、PowerShell 可直接读取。2026-08 之前的版本写 UTF-16LE；升级新版后旧格式文件会在组件启动时自动改名为 `<name>.YYYY-MM-DD.log` 保留，这类历史文件需按 UTF-16 解码查看。

> 若你在 `%ProgramFiles%\FaceLogin\log\` 找不到日志，去 `%ProgramData%\FaceLogin\log\` 看——那是 DataPath 注册表值为空时的回退位置（默认部署不会触发）。查注册表 `HKLM\SOFTWARE\FaceLogin\DataPath` 可确认实际数据目录。

### 2.2 常见问题

| 问题 | 可能原因 | 解决方法 |
|---|---|---|
| 锁屏后短暂显示“正在加载模型” | 锁屏预加载尚未完成或锁屏事件延迟 | 认证请求会兜底等待；检查 `Model load requested: session lock` 与 `Authentication worker ready` 日志间隔 |
| 服务启动失败 | 缺少运行时 DLL | 安装时确保 DLL 与 EXE 同目录 |
| 锁屏不显示磁贴 | 未注册或已禁用 / 无注册用户 | 检查注册表 Disabled 键值，确认已录入人脸 |
| 识别率低 | 光照、距离或注册样本质量不匹配 | 在与注册相近的光线和距离下重试。查看 `service.log` 的 `closest identity distance` 与阈值；查看 `auth_worker.log` 的 `face luma`，持续低于 50 时检查相机朝向和遮挡。当前 SFace 阈值仍处于试用阶段，见 [`SFace 试用说明`](../work/in-progress/sface-trial.md)。 |
| 摄像头不工作 | Session 0 权限、相机占用或驱动错误 | 检查 `auth_worker.log` 的 DirectShow 相机初始化错误，并关闭占用相机的程序 |
| 人脸失败后密码输入被打断 | 部署了会在失败后重新枚举的旧 Credential Provider | 核对已部署 DLL；新版本（2026-08-29 起）失败 tile 无重试按钮、按任意键/点击即重试，会记录 `Terminal failure shown in-place; passive retry re-armed`——监听严格限定本磁贴选中期间，切到密码磁贴即停止；若部署的是更旧版本则应见 `passive retry disabled` 且仅点击“重新尝试人脸识别”才产生新 `AUTH_REQUEST` |
| 锁屏后摄像头指示灯立即点亮 | 旧版服务、其他应用占用摄像头，或 worker 收到异常早到的认证请求 | 当前版本没有摄像头预热开关；核对已部署 EXE 哈希和 `auth_worker.log`，正常顺序必须先 ready，收到认证后才初始化相机 |
| 多次锁屏后内存或句柄持续增长 | 旧服务 EXE 仍在运行，或 worker 没有退出 | 确认每轮成功后 `service.log` 有 `Authentication worker timing ... cleanup=...`，任务管理器无遗留 `FaceLoginService.exe -auth-worker`，并比较父 `service.log` 的 worker-ready 资源值；根因与验收数据见 [`auth-worker-migration-completion.md`](../work/completed/auth-worker-migration-completion.md) |
| 人脸通过后 Windows 拒绝登录 | Windows 账户名或凭据不匹配 | MSA 使用 UPN、本地账户使用 `COMPUTERNAME\Username`；检查当前账户身份，必要时重新注册。数据库兼容规则见 [`users.dat 契约`](../contracts/users-dat.md)。 |

### 2.3 worker 与 DirectShow 诊断

父服务和 worker 分开记录日志。正常流程应满足：

1. `service.log` 出现 `Authentication worker ready in ... ms`；此时默认配置尚未打开摄像头。
2. 收到认证请求后，`auth_worker.log` 才出现相机初始化和认证处理记录。
3. 成功时，`service.log` 出现 `Credentials sent ...`，随后是 `Authentication worker timing ...`。
4. 解锁后任务管理器中只有父 `FaceLoginService.exe`，不存在命令行带 `-auth-worker` 的进程。

worker 在启动、模型、相机、协议或超时阶段失败时应返回错误，并保留密码登录。若 worker 留存，确认已部署当前服务版本，再检查是否存在 `-auth-worker` 进程及服务资源记录是否持续增长。

模型 SHA 不匹配时，`auth_worker.log` 会记录模型名及哈希不匹配；认证按失败处理。恢复正确模型后再次锁屏重试。模型校验细节见 [`face-service.md`](../modules/face-service.md)。

DirectShow 是唯一相机实现。保存配置后通过注册控制台触发重载，或重启服务再锁屏。

开发机可先用以下命令隔离驱动问题，不能把它当作 Session 0 认证结论：

```powershell
.\build\tools\camera_lifecycle\Release\CameraLifecycleTest.exe --mode child --cycles 100 --settle-ms 500
```

每轮必须拿到 10 个 640×480 有效帧；child 模式父进程的句柄、线程和 private bytes 应无随轮次线性增长。
