# FaceLoginSetup 开发文档

FaceLogin 图形化安装/卸载程序（Wails v2 + Vue 3 + Go）。

本文档面向后续维护者，重点说明构建、资源同步与安装流程。

---

## 目录

1. [项目结构](#项目结构)
2. [构建与运行](#构建与运行)
3. [发布新版本的检查清单](#发布新版本的检查清单)
4. [安装流程（后端 Install 各步骤）](#安装流程)
5. [注册表键](#注册表键)

---

## 项目结构

```
FaceLoginSetup/
├── main.go                  # 入口：提权、Wails 启动
├── app.go                   # App 结构体：绑定到前端的方法（Install/Uninstall/...）
├── wails.json               # Wails 项目配置（输出名、平台）
├── go.mod / go.sum
├── internal/                # 与前端无关的后端逻辑
│   ├── config.go            # 当前配置默认值与首次安装创建
│   ├── com.go               # COM DLL 注册/注销
│   ├── scm.go               # Windows 服务安装/停止/删除
│   ├── extract.go           # 内嵌资源解压（resources/ → 安装目录）
│   ├── elevate.go           # 管理员提权
│   └── util.go              # 注册表读写、文件/目录、路径等工具
├── resources/               # 内嵌资源：exe/dll/模型/dll 依赖（编译时打包）
└── frontend/                # Vue 3 + Tailwind 前端
    └── src/
        ├── App.vue          # 全部 UI 逻辑
        ├── main.ts / style.css
        └── wailsjs/         # 自动生成的 Go ↔ JS 绑定（勿手改）
```

---

## 构建与运行

前置：Go 1.25+、Node/npm、Wails CLI v2。

```bash
# 安装 Wails CLI（若未装）
go install github.com/wailsapp/wails/v2/cmd/wails@latest

# 开发模式（前端热重载，后端方法可在浏览器联调）
cd FaceLoginSetup
wails dev

# 生产构建（输出到 build/bin/FaceLoginSetup.exe）
wails build -clean -platform windows/amd64
```

> `wails build` 会自动：生成 Go→JS 绑定（frontend/wailsjs/）、安装前端依赖、编译前端（`vue-tsc --noEmit && vite build`）、编译 Go、将 `resources/` 内嵌打包。**改完 Go 方法或前端后，必须重新 `wails build` 才能生效。**

---

## 发布新版本的检查清单

每次发布新版本安装包时，按此顺序操作：

1. **编译最新 C++ 产物**，并同步到 `resources/`（见下）。
2. `wails build -clean -platform windows/amd64` 重新打包。
3. （可选）验证：把安装包放到一个已安装旧版的环境跑安装，确认替换和卸载路径可用。

### C++ 产物同步

`resources/` 里需要手动同步的二进制（构建不会自动更新）：

| 资源文件 | 来源（MSBuild Release 产物） |
|---|---|
| `FaceLoginService.exe` | `build/face_service/Release/FaceLoginService.exe` |
| `FaceLoginCredentialProvider.dll` | `build/credential_provider/Release/FaceLoginCredentialProvider.dll` |
| `FaceLoginConsole.exe` | `installer/FaceLoginSetup/resources/`（由 MSBuild 自动输出） |
| `onnxruntime.dll` 及 libgcc/lapack 等 | 运行时依赖，通常不变 |
| `models/det_10g_gnkps.onnx` | `assets/models/`（构建 Console 时自动复制并由安装器校验 SHA-256） |
| `models/w600k_r50.onnx` | `assets/models/`（构建 Console 时自动复制并由安装器校验 SHA-256） |
| `models/MiniFASNetV2.onnx` | `assets/models/`（构建 Console 时自动复制并由安装器校验 SHA-256） |
| `models/MiniFASNetV1SE.onnx` | `assets/models/`（构建 Console 时自动复制并由安装器校验 SHA-256） |

> **`FaceLoginConsole.exe` 的构建目标直接输出到 `resources/`**，并从 `assets/models/` 自动准备全部四个运行时模型；服务 EXE 和凭据 DLL 仍需手动复制。`resources/models/` 是生成物，不应直接维护。改 C++ 代码后务必核对时间戳，避免把旧二进制打进安装包。

---

## 安装流程

后端 `App.Install(installDir)`（`app.go`）按序执行，前端通过 `setup:progress` 事件显示进度。模型校验与 ACL 保护分布在关键节点两侧（见提交 `92b0ecc`）：

| 步骤 | 动作 | 备注 |
|---|---|---|
| 0 | 校验安装资源（内嵌模型大小 + SHA-256） | `internal.ValidateEmbeddedResources`；在停止旧服务前执行，尽早发现损坏载荷 |
| 1 | 停止并删除已有服务 | `internal.StopAndDeleteService()` |
| 2 | 创建安装目录 | `os.MkdirAll` |
| 2.5 | 设置安装目录 ACL（预保护） | `internal.SetDirectoryACL`；在写入可执行文件/模型前锁定，使解压文件继承仅 SYSTEM/管理员写权限 |
| 3 | 写注册表 InstallPath / DataPath | 见[注册表键](#注册表键)；DataPath = 安装目录本身 |
| 4 | 解压内嵌资源到安装目录 | `internal.ExtractAll` |
| 4.1 | 校验已复制模型（大小 + SHA-256） | `internal.ValidateInstalledModels`；与步骤 0 相同的固定哈希 |
| **4.5** | **仅在不存在时创建 config.json** | `EnsureConfigDefaults`；旧配置不迁移，要求完整卸载后重装 |
| 5 | 验证目录权限（递归后置 ACL） | `internal.SetDirectoryACL` 递归再校一次，防止解压文件携带意外显式 ACL |
| 6 | 注册 COM DLL | `internal.RegisterCOMDLL` |
| 7 | 安装并启动服务 | `internal.InstallService` |
| 8 | 额外解压 FaceLoginConsole.exe | 结尾补充 |
| — | 返回结果 | 前端据此判断是否查公告（机制二） |

---

## 注册表键

所有注册表操作均位于 `HKLM\SOFTWARE\FaceLogin`：

| 键 | 用途 |
|---|---|
| `InstallPath` | 安装目录（`IsInstalled` / 公告升级判断依据） |
| `DataPath` | 数据目录（C++ 端追加 `\models`、`\data`、`\log`） |

---

## 常见问题

**改了前端/Go 代码但弹窗没变？**
→ 重新 `wails build`。`resources/` 内嵌和前端绑定都在构建时生成/打包。

**公告弹窗没弹，但确认开启了开关？**
→ 检查是否首次安装（无旧版）。可在 `service.log` 或注册表 `InstallPath` 确认已安装状态。

**旧配置没有被迁移？**
→ 这是预期行为：本版本要求完整卸载旧版本后重装，安装器只创建不存在的配置文件。
