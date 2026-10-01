> 2026-10-01：当前安装包为 SFace 试用版：128 维特征、`users.dat` V6、强制重新录入、渐进学习关闭。真实摄像头与锁屏验收状态见 [试用记录](sface-trial.md)。

# Build Guide

Windows x64 构建与打包入口。常规安装包使用 `scripts/build_installer.ps1`；只有需要单独构建组件或排查打包时，才按本页执行手动步骤。

## 前置条件

需要 Visual Studio C++ 工具、CMake、vcpkg、Go、Node.js 和 Wails CLI。vcpkg 首次配置会按根目录 `vcpkg.json` 安装依赖。CMake 配置时使用本机安装的 VS generator 与 vcpkg toolchain；generator 与现有 `build/` 缓存不一致时，先清理该目录。

```powershell
go install github.com/wailsapp/wails/v2/cmd/wails@latest
powershell -ExecutionPolicy Bypass -File scripts/restore_webview2.ps1
```

第二条命令仅在新克隆缺少被忽略的 WebView2 loader library 时需要。

## C++ 构建

在仓库根目录配置并构建 Release。按本机 Visual Studio 版本选择已安装的 generator；CMake 不在 PATH 时用它的完整路径替换 `cmake`：

```powershell
cmake -B build -S . -G 'Visual Studio 18 2026' -DCMAKE_TOOLCHAIN_FILE='C:/vcpkg/scripts/buildsystems/vcpkg.cmake'
cmake --build build --config Release --parallel
```

按本机 Visual Studio 版本选择 generator。默认构建生成 Service、Credential Provider 和 Console；`tools/` 下诊断程序与测试目标需通过 `--target <target>` 显式构建。Console、正式模型和 Console 内许可查看器所需文件会直接复制到安装器资源目录。

主要输出：

| 目标 | 输出 |
|---|---|
| `FaceLoginService` | `build/face_service/Release/FaceLoginService.exe` |
| `FaceLoginCredentialProvider` | `build/credential_provider/Release/FaceLoginCredentialProvider.dll` |
| `FaceLoginConsole` | `installer/FaceLoginSetup/resources/FaceLoginConsole.exe` |

开发测试从根 CMake 单独构建，例如：

```powershell
cmake --build build --config Release --target AuthWorkerProtocolTest CredentialStoreTest
```

相机相关的 `CameraLifecycleTest` 需要真实摄像头。自动化测试不能替代 Session 0、Credential Provider 和锁屏实测。

## 安装包构建

### 推荐：一键构建

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_installer.ps1
```

脚本负责构建 C++、恢复前端依赖、生成许可声明、构建 slim 卸载器、按白名单同步资源、打包载荷、运行安装器测试并生成完整安装包。完成后输出路径、大小、SHA-256 和 ProductVersion。

### 手动构建与核对

资源目录 `installer/FaceLoginSetup/resources/` 被 gitignore，只有 Console 和正式模型会由 CMake 同步，其余文件必须显式准备。手动打包按此顺序：

1. 构建 C++ Release。
2. 运行 `npm --prefix installer/FaceLoginSetup/frontend ci`，再运行 `scripts/update_third_party_notices.ps1`。
3. 在 `installer/FaceLoginSetup/` 构建 slim 卸载器：`wails build -tags slim -platform windows/amd64 -ldflags "-s -w"`；将生成的 EXE 复制到 `resources/uninstall.exe`。
4. 将 Service、Credential Provider 和恰好五个运行时 DLL 复制到 `resources/`。Console 与四个模型已由 CMake 放入该目录。
5. 按下方白名单清理并核对 `resources/`，运行 `scripts/make_payload.ps1`，再于安装器目录执行 `go test ./...`。
6. 运行 `wails build -clean -platform windows/amd64 -ldflags "-s -w"`。

改动 `enrollment_app/index.html` 时须先重建 C++；改动安装器 Go、Vue 或载荷提取逻辑时须重建安装器。单独运行 Wails 不会检测 C++ 资源是否过期。

### 资源白名单

`resources/` 根目录只放以下文件：

- `FaceLoginService.exe`、`FaceLoginCredentialProvider.dll`、`FaceLoginConsole.exe`、`uninstall.exe`
- 五个运行时 DLL：`onnxruntime.dll`、`libprotobuf.dll`、`libprotobuf-lite.dll`、`re2.dll`、`abseil_dll.dll`
- `LICENSE.txt`、`THIRD_PARTY_NOTICES.txt`、`MODEL_LICENSES.txt`

`resources/models/` 只放四个正式 ONNX 模型。离线工具、旧 DLL、临时文件和其他残留都不得打包。新增载荷文件时同步更新静态卸载清单与测试。

服务和 Console 依赖的运行时 DLL 闭包就是上述五个文件；Credential Provider 使用静态 CRT，不需要额外运行时 DLL。DLL 清单或依赖变化时重新检查依赖闭包。

### 许可文件

根目录 `LICENSE`、`THIRD_PARTY_NOTICES.txt` 和 [`third_party/MODEL_LICENSES.txt`](../third_party/MODEL_LICENSES.txt) 是随包许可说明的事实源。详细模型来源核查记录保留在 [`model-licenses.md`](model-licenses.md)。依赖升级后重新生成第三方声明；`scripts/stage_licenses.ps1` 将许可文件复制到安装载荷和前端。注册 Console 页脚的“许可声明”入口从 Console 同目录读取这三份文件。

### 载荷压缩

完整安装器只嵌入 `payload.zip`；`scripts/make_payload.ps1` 从 `resources/` 生成它。资源有任何改动都要重打包。zip 的正斜线路径、显式目录项和 `resources/` 前缀由 `payload_zip_test.go` 覆盖；不要改成直接嵌入资源目录。slim 卸载器不嵌入载荷。

## 前端开发

```powershell
cd installer/FaceLoginSetup/frontend
npm run dev
npm run build
```

`npm run build` 会运行 `vue-tsc --noEmit` 和 Vite 构建。不要手工编辑 `frontend/wailsjs/` 生成绑定。

## 运行与数据目录

生产安装默认将 `DataPath` 设为安装目录；开发运行可用仓库脚本启动 standalone：

```powershell
.\scripts\start_standalone.bat
```

也可直接运行 `build/face_service/Release/FaceLoginService.exe -standalone`。standalone 日志与配置路径等数据目录规则见 [代码导航](../DEVELOPMENT.md#sec-paths)；运维排障见 [运维手册](operations/operations.md)。worker 只在认证请求后打开相机。

## 版本号与版本资源

用 `scripts/bump_version.ps1 <x.y.z[-suffix]>` 更新版本号；加 `-Dry` 可预览。脚本同步 CMake、Wails、README 和构建资源所需版本位置，并校验替换数量。

CMake 产物从根 `project()` 版本生成 VERSIONINFO。Wails 的 `build/windows/info.json` 中语言 ID 应为 `0409`。可用以下命令核对四个 EXE/DLL 的版本资源：

```powershell
(Get-Item <exe>).VersionInfo.ProductVersion
```

## 摄像头曝光诊断（开发工具）

需要时显式构建探针。它会打开指定相机并在正常退出时恢复曝光、增益及自动/手动模式；测量期间不要强制结束进程，也不要移动相机或改变光线。

```powershell
cmake --build build --config Release --target ExposureResponseProbe FaceGainTuneTest
.\build\tools\camera_lifecycle\Release\ExposureResponseProbe.exe --list
.\build\tools\camera_lifecycle\Release\ExposureResponseProbe.exe --camera-index 1 > build/exposure-response.csv
.\build\tools\face_gain_tune\Release\FaceGainTuneTest.exe build/exposure-response.csv
```

探针输出仅用于调节器诊断，不替代人脸认证或 Session 0 验证，也不会进入安装包。
