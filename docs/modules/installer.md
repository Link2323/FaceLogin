# 安装器模块

目录：`installer/FaceLoginSetup/`

`FaceLoginSetup.exe` 使用 Go + Wails v2 + Vue 3，仅负责部署和卸载，不参与运行期命名管道。完整构建命令和资源同步步骤以 [`docs/BUILD.md`](../BUILD.md) 为唯一事实源。

## 代码入口

| 位置 | 责任 |
|---|---|
| `app.go` | 安装/卸载编排、进度事件、目录选择 |
| `internal/extract.go` | `embed.FS` 载荷枚举、提取、模型大小/SHA 校验 |
| `internal/com.go` | COM 注册/注销、安装目录 ACL |
| `internal/security.go` | `HKLM\SOFTWARE\FaceLogin` 注册表键 ACL |
| `internal/scm.go` | 服务停止、删除、安装、启动 |
| `internal/config.go` | 默认 config 创建和选择性迁移 |
| `internal/util.go` | 注册表值、路径/文件和命令辅助 |
| `frontend/src/App.vue` | Wails 安装/卸载 UI |
| `resources/` | 待嵌入部署载荷镜像，不是事实源 |

`frontend/wailsjs/` 是生成绑定，不手工编辑。

## 安装顺序

`App.Install` 当前顺序：

1. 校验内嵌四模型的固定大小和 SHA-256，失败时不停止旧服务。
2. 停止并删除旧服务。
3. 结束残留的 `FaceLoginService.exe`（SCM 停止后的散落 standalone/认证 worker）与 `FaceLoginConsole.exe`（上次安装完成页自动启动残留），否则更新场景下 exe 镜像锁导致 `ExtractAll` 写入失败；`internal.KillProcessesByName` 结束后等待进程对象信号，确认锁释放。
4. 创建目标目录。
5. `SetDirectoryACL` 预保护目录，避免写入窗口。
6. 写入 `InstallPath` / `DataPath`；DataPath 等于安装目录。
7. `SetRegistryKeyACL` 保护 FaceLogin 注册表键。
8. `ExtractAll` 提取嵌入载荷。
9. 再次校验落盘模型。
10. 创建 `data/`、`log/` 并迁移/创建 `config.json`。
11. 递归重施目录 ACL 作为后置条件；随后 `SetDataDirectoryACL` 把 `data/` 子树单独收紧为仅 SYSTEM+Administrators(无 Users 条目)——`users.dat` 的 DPAPI 机器作用域密文任何本地账户都能解,文件 ACL 是非管理员进程前的唯一屏障,且必须排在整目录 `/T` 复扫之后,否则 Users 条目会被刷回。对应回归测试 `internal/com_test.go`。
12. 注册 Credential Provider COM DLL。
13. 安装并启动 Windows 服务。
14. 最终再次覆盖 `FaceLoginConsole.exe`；这是当前实现保留的冗余写入。
15. 写入"应用列表"(Add/Remove Programs)注册表条目 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\FaceLogin`（DisplayName/Version/UninstallString/DisplayIcon/NoModify/NoRepair），`UninstallString` 指向随载荷部署的 `uninstall.exe`；失败仅 warn 不影响安装结果。
16. 前端完成页（复选框+完成按钮）行为见上；`uninstall.exe` 是 slim 构建（`wails build -tags slim`，无嵌入载荷，~11 MB），始终落在卸载页并隐藏模式 Tab；完整安装器也接受 `--uninstall` 参数直达卸载页。slim 卸载自身走分离隐藏 cmd 延迟自删（`internal.SelfDelete`），重启删除队列作兜底。

调整顺序时优先保护这三个性质：损坏载荷不破坏可工作的旧安装、可执行文件写入前目录已受保护、启动 SYSTEM 服务前模型已在磁盘校验。

## 卸载清单来源

完整安装器优先枚举嵌入资源决定删除哪些文件；slim `uninstall.exe` 无嵌入资源，退回 `internal/extract.go` 的静态清单 `currentRootPayloadFiles`（含 `uninstall.exe` 自身）+ `requiredModels`——新增载荷文件时两处（嵌入枚举隐式覆盖 + 静态清单）都要确认。

## 载荷机制

**正式载荷白名单只在 `docs/BUILD.md` 维护，本文件不复制文件清单。**

安装器完整构建只嵌入 `payload.zip`（`resources_full.go` 的 `//go:embed payload.zip`），由 `scripts/make_payload.ps1` 从 `resources/` 打包（91 MB → ~44 MB）。`resources/` 中的任何意外文件都会被打进 zip 随包发布；该目录按文件类型被 gitignore，当前本地内容可能是旧残留，不能反向作为文档或发布清单。每次打包前按 BUILD 指南核对根文件和 `resources/models/`，并清除清单外工具、旧 DLL 和模型中间文件。`resources/` 任何变动后必须重新运行打包脚本——zip 过期不会报错，只会装出旧载荷。

zip 通过 `archive/zip` 的 `fs.FS` 视图读取，所有 `resources/...` 查找路径、模型 SHA-256 校验与卸载枚举逻辑不变；条目 CRC32 在每次读取时附加校验。zip 的三个结构性质（正斜杠分隔、显式目录条目、`resources/` 前缀）由 `payload_zip_test.go` 锁定。

`ExtractAll` 会枚举资源根目录和模型子目录再落盘；增加子目录或改变布局时不能只修改打包与 embed 声明，还要同步提取、卸载和安装后校验路径。slim 卸载构建不嵌入 zip。

## 模型策略

当前工作区四个正式模型已下载并校验。普通构建/打包复用现有 `assets/models/` 和 `resources/models/`，不要重新下载或量化。

只有以下情况运行模型准备脚本：

- 新克隆确实缺少模型
- SHA-256 校验失败且确认需要恢复
- 任务明确要求升级/替换模型

换模型时必须同步 C++ hash、Go `requiredModels`、下载/转换脚本、模型大小、标定文档和安装器测试。

## 安装后目录

```text
<InstallPath>/                 # 同时是 DataPath
├── 三个产品二进制 + 五个 DLL
├── data/
│   ├── config.json
│   └── users.dat
├── log/
│   ├── service.log
│   ├── credential_provider.log
│   └── console.log
└── models/
    └── 四个正式 ONNX
```

默认 `<InstallPath>` 为 `C:\Program Files\FaceLogin`，用户可选择其他安装目录。ProgramData 只是空 DataPath 或开发场景的回退，不是默认生产位置。

## 卸载语义

卸载是完全清除：

1. 停止并删除服务。
2. 尽力结束残留的 `FaceLoginService.exe` / `FaceLoginConsole.exe`，让文件删除即时完成而不是排入重启队列；失败仍由重启删除兜底。
3. 注销 COM DLL。
4. 删除安装器拥有的程序文件以及 `data/`、`log/`。
5. 删除 FaceLogin 注册表键。
6. 仅在安装目录变空时移除目录本身。

`RemoveInstalledFiles` 只删除已知载荷和明确的用户数据路径，避免恶意/损坏 InstallPath 导致任意递归删除。删除结果区分立即删除、经 `MoveFileEx` 验证成功的重启删除和真实失败；待重启时连同安装目录一起排入删除队列。未知文件会明确报错、保留目录和 `InstallPath`，不会再被误报为卸载成功。

## 验证

在 `installer/FaceLoginSetup/`：

```text
go test ./...
```

前端改动还要在 `frontend/`：

```text
npm run build
```

需要生成安装包时再按 `docs/BUILD.md` 运行 Wails 构建。至少检查：

- 资源白名单无多余文件
- 四模型 pre/post extract 校验
- 升级保留未强制迁移的配置
- 安装失败时旧服务影响
- ACL 与注册表路径
- COM 注册和服务启动
- 卸载提示、完全清除和未知文件保留
