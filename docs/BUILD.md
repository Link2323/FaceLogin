> 当前识别模型、数据兼容要求和真实环境验收状态见 [SFace 试用记录](work/in-progress/sface-trial.md)。

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

开发测试从根 CMake 单独构建。无硬件自动化回归使用统一入口：

```powershell
cmake --build build --config Release --target check --parallel 1
```

`check` 先构建再通过 CTest 运行 11 个自动化测试，失败返回非零。只重跑已构建测试可用 `ctest --test-dir build -C Release --output-on-failure -L automated`。相机探针、标定、GUI 与锁屏实测不纳入此目标；普通产品构建和安装器打包不会运行测试。


`AuthenticationResidencyTest` 直接运行生产驻留状态机，以确定性 worker 替身覆盖预载中解锁、使用期间解锁、重载版本确认及失败、重载中解锁、加载失败重试、截止时间、worker 提前退出、终态补建策略和停止期间引用释放。测试不创建真实子进程或打开相机，不能替代进程/Job 故障注入及 Session 0 验收。

服务和注册程序通过静态库复用共享实现：`vision/` 提供 `facelogin_camera`（DirectShow）与 `facelogin_inference`（ONNX Runtime），`storage/` 提供 `facelogin_credential_store`。曝光状态文件读写由独立的 `facelogin_tune_state` 静态库提供，不依赖相机或推理。消费方链接目标即可获得头文件和传递依赖，不再把这些 `.cpp` 加入各自源码清单。相机诊断程序只链接相机库；`AuthPipelineTest` 保留确定性推理替身，不链接真实推理实现。Credential Provider 仅引用存储头文件中的定义，继续使用自己的静态 CRT 构建。

注册端支持模块可独立验证，无需打开相机或启动已安装的服务：

```powershell
cmake --build build --config Release --target EnrollmentSupportTest IpcProtocolTest --parallel 1
.\build\tools\enrollment_support\Release\EnrollmentSupportTest.exe
.\build\tools\ipc_protocol\Release\IpcProtocolTest.exe
```

`EnrollmentSupportTest` 使用独立命名管道和自动清理的临时日志目录，覆盖服务请求/ACK、拒绝与断开响应，以及日志编码、截尾和正在写入的日志清空。

## 安装包构建

### 推荐：一键构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_installer.ps1
```

脚本只负责产品构建与打包：显式串行构建 `FaceLoginService`、`FaceLoginCredentialProvider`、`FaceLoginConsole`，恢复前端依赖、生成许可声明、构建 slim 卸载器、按白名单同步资源和已有模型、压缩载荷并生成完整安装包。前端的 TypeScript 编译属于产品构建。完成后输出路径、大小、SHA-256 和 ProductVersion。

### 构建环境排障

- **MSBuild 的 `PATH` / `Path` 重复键或并行失败**：一键脚本在进程内归一化环境变量大小写（合并 PATH 条目），并固定 `--parallel 1`；不修改用户或系统环境。并行失败的底层原因尚未确认，串行是已验证的规避方式。
- **npm 报 `spawn EPERM` 或缓存无法写入**：脚本默认使用 `build/npm-cache`，保留已设置的 `NPM_CONFIG_CACHE`。构建宿主仍须允许 npm/Node 启动子进程及 Go/npm 缓存写入；Codex 内应在调用完整脚本时使用允许构建的执行权限。脚本不能自行绕过宿主沙箱。
- **Windows PowerShell 5.1 继承 PowerShell 7 的模块路径**：脚本在 Desktop 版恢复标准模块搜索目录，避免 `Get-FileHash` 等内置命令无法加载。
- **Wails `-clean` 删除旧产物后受文件锁影响**：一键脚本不再使用 `-clean`，保留构建产物的新鲜度判断；失败立即停止，不自动重复整条构建流程。

### 手动构建与核对

资源目录 `installer/FaceLoginSetup/resources/` 被 gitignore，只有 Console 和正式模型会由 CMake 同步，其余文件必须显式准备。手动打包按此顺序：

SFace 自 2026-10-04 起使用清理 initializer graph inputs 的规范化 FP32 文件，运行哈希与原始上游不同；文件名和载荷数量不变。普通构建复用 `assets/models/` 已验证文件；确需补模型时，下载器验证上游、规范化并验证衍生哈希后才替换。不得把原始上游文件直接放入载荷。

1. 显式串行构建产品目标：`cmake --build build --config Release --target FaceLoginService FaceLoginCredentialProvider FaceLoginConsole --parallel 1`。
2. 运行 `npm --prefix installer/FaceLoginSetup/frontend ci`，再运行 `scripts/update_third_party_notices.ps1`。
3. 在 `installer/FaceLoginSetup/` 构建 slim 卸载器：`wails build -tags slim -platform windows/amd64 -ldflags "-s -w"`；将生成的 EXE 复制到 `resources/uninstall.exe`。
4. 将 Service、Credential Provider 和恰好五个运行时 DLL 复制到 `resources/`。Console 与四个模型已由 CMake 放入该目录。
5. 按下方白名单清理并核对 `resources/`，运行 `scripts/make_payload.ps1`。
6. 运行 `wails build -platform windows/amd64 -ldflags "-s -w"`。

开发验证与打包分开：安装器 Go 代码变更后在 `installer/FaceLoginSetup/` 单独运行 `go test ./...`，C++ 测试按目标显式构建和运行。

注册页面源文件为 `enrollment_app/index.html.in`、`styles.css` 和 `enrollment_app/CMakeLists.txt` 中 `ENROLLMENT_JS_SOURCES` 列出的 11 个 JS 模块（位于 `enrollment_app/js/`，`capture_flow.js` 到 `app_main.js`，列表顺序即拼接与依赖顺序）；CMake 合成为 `build/enrollment_app/index.html`，由生成的 `resource.rc` 嵌入 EXE，不增加运行期资源文件。修改任一源文件后须重建 C++；改动安装器 Go、Vue 或载荷提取逻辑时须重建安装器。单独运行 Wails 不会检测 C++ 资源是否过期。

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

注册页面采集流程的无硬件回归：`node tools/enrollment_ui/capture_flow_test.cjs`。该测试覆盖角度间取消、立即重试和旧回调隔离；实际 WebView2、摄像头与保存流程仍需 GUI 验收。

```powershell
cd installer/FaceLoginSetup/frontend
npm run dev
npm run build
```

`npm run build` 会运行 `vue-tsc --noEmit` 和 Vite 构建。不要手工编辑 `frontend/wailsjs/` 生成绑定。

## 运行与数据目录

生产安装默认将 `DataPath` 设为安装目录。完整认证通过安装后的 Windows 服务与 worker 验证；前台 standalone 入口已移除。服务启动、停止及排障见 [运维手册](operations/operations.md)。worker 只在认证请求后打开相机。

开发时先运行无硬件回归与独立相机诊断工具；完整登录/解锁验收需部署到测试机，由 SCM 启动服务。路径规则见 [代码导航](../DEVELOPMENT.md#sec-paths)。

## 版本号与版本资源

用 `scripts/bump_version.ps1 <x.y.z[-suffix]>` 更新版本号；加 `-Dry` 可预览。脚本同步 CMake、`main.go`、README、`wails.json` 和 `info.json` 五处版本位置（`info.json` 仅存在于磁盘），并校验替换数量。

CMake 产物从根 `project()` 版本生成 VERSIONINFO。Wails 的 `build/windows/info.json` 中语言 ID 应为 `0409`。可用以下命令核对四个 EXE/DLL 的版本资源：

```powershell
(Get-Item <exe>).VersionInfo.ProductVersion
```
