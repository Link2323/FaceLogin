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
| `console.log` | 注册控制台(原名 enrollment.log,v1.7.x 起更名,首次启动自动迁移) |

> 日志文件为 **UTF-8（无 BOM）**，记事本、`grep`、PowerShell 可直接读取。2026-08 之前的版本写 UTF-16LE；升级新版后旧格式文件会在组件启动时自动改名为 `<name>.YYYY-MM-DD.log` 保留，这类历史文件需按 UTF-16 解码查看。

> 若你在 `%ProgramFiles%\FaceLogin\log\` 找不到日志，去 `%ProgramData%\FaceLogin\log\` 看——那是 DataPath 注册表值为空时的回退位置（默认部署不会触发）。查注册表 `HKLM\SOFTWARE\FaceLogin\DataPath` 可确认实际数据目录。

### 2.2 常见问题

| 问题 | 可能原因 | 解决方法 |
|---|---|---|
| 锁屏后短暂显示“正在加载模型” | 锁屏预加载尚未完成或锁屏事件延迟 | 认证请求会兜底等待；检查 `Model load requested: session lock` 与 `Authentication worker ready` 日志间隔 |
| 服务启动失败 | 缺少运行时 DLL | 安装时确保 DLL 与 EXE 同目录 |
| 锁屏不显示磁贴 | 未注册或已禁用 / 无注册用户 | 检查注册表 Disabled 键值，确认已录入人脸 |
| 识别率低 | 光照不足 / 嵌入质量差 | 重新注册人脸，确保光线均匀（注册与解锁的光照/距离条件要一致——夜间注册白天解锁会把真脸距离推到阈值边缘）。失败日志可定位：`service.log` 失败 WARN 的 `closest identity distance` 接近 `threshold` 为采集条件问题、远高于阈值为非本人；`auth_worker.log` 的 `face luma` 低于 ~40 即欠曝光（`low_light_enhance` 默认关闭，2026-08-31 至 2026-09-05 期间曾默认开启；欠曝光场景可在设置中或改 config.json 手动开启） |
| 摄像头不工作 | Session 0 权限、相机占用或驱动错误 | 检查 `auth_worker.log` 的 DirectShow 相机初始化错误，并关闭占用相机的程序 |
| 人脸失败后密码输入被打断 | 部署了会在失败后重新枚举的旧 Credential Provider | 核对已部署 DLL；新版本（2026-08-29 起）失败 tile 无重试按钮、按任意键/点击即重试，会记录 `Terminal failure shown in-place; passive retry re-armed`——监听严格限定本磁贴选中期间，切到密码磁贴即停止；若部署的是更旧版本则应见 `passive retry disabled` 且仅点击“重新尝试人脸识别”才产生新 `AUTH_REQUEST` |
| 锁屏后摄像头指示灯立即点亮 | 旧版服务、其他应用占用摄像头，或 worker 收到异常早到的认证请求 | 当前版本没有摄像头预热开关；核对已部署 EXE 哈希和 `auth_worker.log`，正常顺序必须先 ready，收到认证后才初始化相机 |
| 多次锁屏后内存或句柄持续增长 | 旧服务 EXE 仍在运行，或 worker 没有退出 | 确认每轮成功后 `service.log` 有 `Authentication worker timing ... cleanup=...`，任务管理器无遗留 `FaceLoginService.exe -auth-worker`，并比较父 `service.log` 的 worker-ready 资源值；根因与验收数据见 [`auth-worker-migration-completion.md`](../auth-worker-migration-completion.md) |
| 人脸登录后用户名密码错误 | MSA 账户凭据格式不对 | 确认 V4 数据库含正确 UPN |
| 注册时显示空白 UPN | MSA 账户 GetUserNameExW 失败 | 已通过 IdentityStore 回退解决 |

### 2.3 worker 与 DirectShow 诊断

父服务和 worker 分开记录日志。一次正常的锁屏认证应满足：

1. `service.log` 出现 `Authentication worker ready in ... ms`；此时默认配置尚未打开摄像头。
2. `auth_worker.log` 依次出现 worker 启动横幅、推理预热以及（收到认证后）相机初始化与 PAD 证据完成；worker 侧不记 ready 资源行——资源快照只由父进程的 `Authentication worker ready` 记录（2026-08-31 日志精简）；成功关键路径不在 worker 内同步写计时或终态日志。
3. `service.log` 先出现 `Credentials sent ...`，随后出现 `Authentication worker timing: camera_init=..., pipeline=..., total=...; supervisor=..., cleanup=...`。这些计时由 worker 的私有成功终态带回，并在公共凭据已写出后才落日志；`cleanup` 是凭据投递（含 ACK）之后收割 worker 进程的耗时——收割刻意排在投递之后，进程拆除等待（慢机实测 15–40ms）不得挡在识别结论与锁屏之间。
4. 解锁后任务管理器中只有父 `FaceLoginService.exe`，不存在命令行带 `-auth-worker` 的进程。

worker 在启动、模型、相机、协议或超时阶段失败时都只能向锁屏返回明确错误并允许密码登录；不要把故障规避成父进程内人脸认证。若 worker 留存，先确认部署的是包含 Job 生命周期实现的新 EXE，再看服务日志中的父进程句柄/private bytes 是否随轮次线性增长。

模型 SHA 不匹配时，`auth_worker.log` 会在对应模型名下记录 expected/got hash 和 `tampered or corrupted; treating as fail-closed`，父服务只向锁屏返回模型加载错误并保留密码登录。恢复正确模型后，下一次锁屏或显式认证请求会创建新 worker 并重新校验全部四个模型；不要尝试在父服务内绕过校验继续认证。

DirectShow 是唯一相机实现；`camera_backend` 已不再是配置字段，旧配置中的同名字段会被忽略。保存配置后通过注册控制台触发 `CONFIG_RELOAD`，或重启服务并重新锁屏。2026-08-13 的历史 Session 0 对比表明另一条实验路径整体更慢，因此该实现及其 A/B 开关已删除。

开发机可先用以下命令隔离驱动问题，不能把它当作 Session 0 认证结论：

```powershell
.\build\tools\camera_lifecycle\Release\CameraLifecycleTest.exe --mode child --cycles 100 --settle-ms 500
```

每轮必须拿到 10 个 640×480 有效帧；child 模式父进程的句柄、线程和 private bytes 应无随轮次线性增长。
