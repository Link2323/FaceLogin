# FaceLogin 待办

只记录尚未完成的工作。已完成的 worker 迁移、性能 A/B、长期资源验收和发布验证统一见 [`auth-worker-migration-completion.md`](../completed/auth-worker-migration-completion.md)；不要在本文件重复历史实验和已关闭问题。

难度标记与风险优先级独立：**C** 为局部小改动或脚本，**B** 为单模块改造并补测试，**A** 为跨模块/跨语言契约改动，**S** 为 Windows 多线程、COM 或 I/O 生命周期重构。

## 安全与运维

### B — 日志与安装目录 ACL 收紧

`service.log` / `credential_provider.log` 仍沿用安装目录的 `Users:RX`。其中的 SID、用户名和距离分数可能有助于侧信道标定。

建议：生产日志将匹配距离降为 DEBUG，并将日志目录 ACL 收紧为 SYSTEM + Owner；同时验证服务、Credential Provider、Enrollment 仍可正常写日志。

### A — 模型 hash 跨语言单一来源

C++ 的生产 worker、standalone 和 `ModelIntegrityTest` 已共同使用 `common/model_hashes.h`；Go 安装器、PAD 标定工具与模型下载脚本仍维护各自的 hash 常量，模型升级时可能漂移。

建议：引入机器可读 manifest，并生成或统一校验 C++、Go 和 PowerShell 使用的值；至少先由脚本和安装器测试共同校验 64 位十六进制格式与四个正式模型。

### A — DataPath 解析统一

`ResolveSecureDataDir` 的白名单目前只在服务端执行；Credential Provider 与 Enrollment 仍直接读取 `DataPath`。安装器默认将其设为安装目录且注册表 ACL 已受保护，当前没有已知绕过，但三组件的规则不一致。

建议：在保留 Credential Provider 的 ProgramData 空值回退语义下，让三组件共用相同的受信路径解析与测试。
