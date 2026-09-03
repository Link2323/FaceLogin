# Build Guide

Compilation reference for FaceLogin. AGENTS.md keeps a one-line pointer to this file; open this when you actually need to build. All commands verified working 2026-08-13 via a clean end-to-end rebuild and installer repack.

Shell examples are Git Bash (this machine's default). Forward slashes, `/c/` `/d/` drive roots, `/dev/null`.

## Prerequisites

[vcpkg](https://vcpkg.io) installed (this machine: `C:\vcpkg`), Visual Studio Build Tools with the C++ workload, Go, Node.js, and the Wails CLI (`go install github.com/wailsapp/wails/v2/cmd/wails@latest`). This machine has vcpkg's CMake on the **user PATH** (added 2026-08-13), so plain `cmake` resolves directly; on a machine without it, set the variable:

```bash
CMAKE="C:/vcpkg/downloads/tools/cmake-4.4.0-windows/cmake-4.4.0-windows-x86_64/bin/cmake.exe"
```

**Generator — check first:** `$CMAKE -G` lists available generators. This machine has VS 2026 Build Tools → use `Visual Studio 18 2026` (older machines use `Visual Studio 17 2022`). If the generator in the cache doesn't match, delete `build/` first.

**Parallelism (re-verified 2026-08-13 on this 32-core machine):** the root `CMakeLists.txt` enables `/MP` (file-level parallelism) for all MSVC versions, and writes `TrackFileAccess=false` + `ErrorReporting=None` into each `.vcxproj` for VS 2026 — the combination that keeps `/MP` stable (no orphaned `cl.exe`; verified with repeated clean rebuilds). Baseline clean-build timings: serial 39 s → `/MP` alone 13.8 s → `/MP` + `--parallel` 7.8 s. The default command already benefits; add `--parallel` for the last 1.8×.

**Known build trap — WebView2 loader lib:** `enrollment_app/webview2/lib/WebView2LoaderStatic.lib` is gitignored (`*.lib`) and not committed, so a fresh clone fails to link `FaceLoginConsole` with `LNK1181`. CMake now fails at configure time with the exact command when it is missing; restore it once:

```bash
powershell -ExecutionPolicy Bypass -File scripts/restore_webview2.ps1
```

## C++ Build (CMake + vcpkg + MSVC)

Dependencies install from the repo's `vcpkg.json` manifest on first configure (~15 min, cached afterwards):

```bash
"$CMAKE" -B build -S . -G "Visual Studio 18 2026" -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
"$CMAKE" --build build --config Release          # /MP already on; add --parallel for ~1.8× more
```

默认构建只生成三个运行时产物；`tools/` 下的开发诊断与测试都以
`EXCLUDE_FROM_ALL` 注册，需按 target 显式构建，例如：

```bash
"$CMAKE" --build build --config Release --target CredentialStoreTest
```

Targets (incremental is fine when only sources changed; `rm -rf build` for a truly clean rebuild):

| Target | Output |
|---|---|
| `FaceLoginService` | `build/face_service/Release/FaceLoginService.exe` |
| `FaceLoginCredentialProvider` | `build/credential_provider/Release/FaceLoginCredentialProvider.dll` |
| `FaceLoginConsole` | `installer/FaceLoginSetup/resources/FaceLoginConsole.exe` ← **directly** (custom output dir) |
| `PadCalibration` | `build/tools/pad_calibration/Release/PadCalibration.exe` |

`PadCalibration`, `EmbeddingTest`, `CameraLifecycleTest`, `PipeLifecycleTest`, `IpcProtocolTest`, `AuthWorkerProtocolTest`, `AuthWorkerLifecycleTest`, `CredentialStoreTest`, `CredentialProviderInteractionPolicyTest`, `ModelIntegrityTest`, `LivenessTimingTest`, and `ExposureWarmupTest` under `build/tools/*/Release/` are dev-only diagnostics/tests: they are not part of the installer and are not included in the default build. `facelogin_common` is a static library, not a standalone target.

## C++ development verification

Run from the repository root after a Release build:

```powershell
.\build\tools\auth_worker_protocol\Release\AuthWorkerProtocolTest.exe
.\build\tools\ipc_protocol\Release\IpcProtocolTest.exe
.\build\tools\credential_store\Release\CredentialStoreTest.exe
.\build\tools\credential_provider_policy\Release\CredentialProviderInteractionPolicyTest.exe
.\build\tools\model_integrity\Release\ModelIntegrityTest.exe
.\build\tools\auth_worker_lifecycle\Release\AuthWorkerLifecycleTest.exe --cycles 100
.\build\tools\camera_lifecycle\Release\CameraLifecycleTest.exe --mode child --cycles 100 --settle-ms 500
.\build\tools\pipe_lifecycle\Release\PipeLifecycleTest.exe      # overlapped 接受/读写/取消、ACK、ACL、超时与远程拒绝；--verify-30s 追加 30 秒默认截止
.\build\tools\liveness_timing\Release\LivenessTimingTest.exe
.\build\tools\exposure_warmup\Release\ExposureWarmupTest.exe            # 纯状态机；--camera 用真实 DirectShow 相机跑预热循环
```

`AuthWorkerProtocolTest` covers private worker serialization and fail-closed validation. `IpcProtocolTest` locks the public Credential Provider wire format, including MSA, local-account empty UPN, passwords containing colons, rejected legacy success messages, all terminal types, delivery ACK values and the bounded ACK deadline. `PipeLifecycleTest` exercises the production overlapped server and CP client, including connect/write/read ordering, prompt cancellation, terminal-callback self-destruction, silent-client deadlines, immediate-write accept races, remote rejection and ACL/private-byte stability. `CredentialStoreTest` covers V4 loading, rejected old versions, transactional malformed reload, identity-only matching and final DPAPI decrypt. `CredentialProviderInteractionPolicyTest` covers waiting/retry watcher eligibility, failure re-enumeration guards, ordinary wallpaper BREAK handling, mouse-button-only triggering, all three Win+L long-hold release orders, the conservative first-L drain, and held-input rejection across retry rounds; password keystrokes stay excluded structurally (watcher stopped on deselect, no editable field in the tile). `ModelIntegrityTest` verifies canonical/case-insensitive SHA-256 acceptance plus mismatch and missing-file fail-closed behavior without touching installed models. `AuthWorkerLifecycleTest` uses a mock private worker but exercises the production `AuthWorkerClient`, inherited handles, timeout and Job cleanup. `CameraLifecycleTest` requires a real camera; each cycle must obtain at least ten valid 640×480 frames. `--mode inproc` intentionally reproduces backend-local behavior, while `--mode child` is the production resource-boundary check. These automated checks do not replace Credential Provider/Session 0 face authentication. `LivenessTimingTest` locks the anti-spoof fast-fail timing state machine: the empty-scene timer times from the PAD window start, the persistent-attack timer from the first detected face, and the attack fast fail stays off after the first PAD pass. `ExposureWarmupTest` locks the adaptive exposure-warmup gate: a settled scene opens after 2 distinct samples, ramps extend to the first stable window, oscillation caps at 10 samples, and an outlier inside the window delays the exit until it rolls out; `--camera` mirrors the AuthPipeline warmup loop against the real DirectShow camera and asserts frame-sequence monotonicity.

## Full Installer Rebuild

The shippable installer is `installer/FaceLoginSetup/build/bin/FaceLoginSetup.exe` (size varies with embedded models).

**One command (2026-09-02, verified end to end, ~1 min):**

```bash
powershell -ExecutionPolicy Bypass -File scripts/build_installer.ps1
```

It chains everything below (CMake → slim uninstaller → resource sync → payload.zip → go test → full wails build) and additionally enforces what the manual chain only asks you to remember: `resources/` is pruned against the payload allow-list before packing, the `wails build -clean` transient-lock failure is retried once without `-clean`, and success requires the output exe to be newer than the script's start (no-op build guard). Prints path/size/SHA-256/ProductVersion at the end. The manual steps remain below as the reference for what the script does.

```bash
# 1. C++ build
"$CMAKE" --build build --config Release

# 2. Build the SLIM UNINSTALLER first (~11 MB, -tags slim, no embedded payload)
#    and stage it into resources/ — the full build embeds it as payload:
wails build -tags slim -platform windows/amd64 -ldflags "-s -w"
cp installer/FaceLoginSetup/build/bin/FaceLoginSetup.exe installer/FaceLoginSetup/resources/uninstall.exe

# 3. Sync artifacts into resources/ (FaceLoginConsole.exe outputs directly there — never copy it;
#    the 5 runtime DLLs are NOT auto-deployed there either, they must be copied from face_service/Release)
cp build/face_service/Release/FaceLoginService.exe                installer/FaceLoginSetup/resources/
cp build/credential_provider/Release/FaceLoginCredentialProvider.dll installer/FaceLoginSetup/resources/
cp build/face_service/Release/*.dll                               installer/FaceLoginSetup/resources/

# 4. Pack resources/ into payload.zip (91 MB → ~44 MB; the full build embeds
#    ONLY this zip, never the bare directory — see the note below)
powershell -ExecutionPolicy Bypass -File scripts/make_payload.ps1

# 5. Validate the packed payload (model SHA-256s + zip enumerability + extraction)
cd installer/FaceLoginSetup && go test ./...        # expect: ok FaceLoginSetup

# 6. Wails build (~57 MB, embeds payload.zip + Vue frontend + Go backend)
cd .. && wails build -clean -platform windows/amd64 -ldflags "-s -w"   # → build/bin/FaceLoginSetup.exe
```

### `resources/` is entirely hand-synced

Nothing in CMake copies DLLs or exes into `resources/` except: (a) `FaceLoginConsole.exe`, whose `RUNTIME_OUTPUT_DIRECTORY_RELEASE` is set directly to `resources/` (`enrollment_app/CMakeLists.txt:56`), and (b) the 4 ONNX models, copied by a `FaceLoginConsole` POST_BUILD command from `assets/models/`. **Everything else — `FaceLoginService.exe`, `FaceLoginCredentialProvider.dll`, all 5 runtime DLLs, and `uninstall.exe` — must be copied manually** (the uninstaller comes from the step-2 slim build). `resources/` is gitignored, so verify its contents before every Wails build.

The root payload allow-list is exactly the three product binaries (`FaceLoginService.exe`, `FaceLoginCredentialProvider.dll`, `FaceLoginConsole.exe`), the slim `uninstall.exe` (staged from the `-tags slim` build; it is what the Add/Remove Programs entry launches), plus the five runtime DLLs listed below; `resources/models/` contains exactly the four production models. `PadCalibration.exe`, `EmbeddingTest.exe`, old DLLs, and other local leftovers are development artifacts and must not be shipped — anything left in `resources/` ends up inside `payload.zip` and ships.

### Payload compression — `payload.zip`, never the bare directory

The full installer embeds **only `payload.zip`** (`//go:embed payload.zip` in `resources_full.go`), produced by `scripts/make_payload.ps1`: 91 MB of payload → ~44 MB, so the installer ships at ~57 MB instead of ~107 MB. The zip is read through `archive/zip`'s `fs.FS` view, so all existing `resources/...` paths, model SHA-256 validation and uninstall enumeration work unchanged; zip entry CRC32s are verified on every read on top. Three properties of the zip are load-bearing and locked by `payload_zip_test.go`:

- entries use **forward slashes** (`.NET ZipFile.CreateFromDirectory` writes backslashes — the reason the script builds entries manually; a backslash zip silently breaks `fs.ReadDir`/`Open`);
- **explicit directory entries** exist for every folder (fs.FS cannot enumerate directory entries that only exist implicitly);
- the `resources/` path prefix is preserved (the Go code looks everything up as `resources/...`).

`payload.zip` is gitignored; regenerate it after ANY change to `resources/`. The slim uninstaller build (`-tags slim`) does not embed the zip at all.

### Runtime DLL set — copy from `build/face_service/Release/` (exactly 5, do not deviate)

`FaceLoginService.exe` and `FaceLoginConsole.exe` statically link onnxruntime's C++ wrapper (use `Ort::Session` etc. in `face_service/onnx_models.cpp`), so `onnxruntime.dll` is in their import table. Its transitive closure (verified via `dumpbin /dependents` recursion) is exactly: `onnxruntime.dll`, `libprotobuf.dll`, `libprotobuf-lite.dll`, `re2.dll`, `abseil_dll.dll` (abseil only adds system `bcrypt.dll`).

The `libgcc_s_seh-1.dll` / `libgfortran-5.dll` / `liblapack.dll` / `libquadmath-0.dll` / `libwinpthread-1.dll` / `openblas.dll` that appear in some build subdirs are **build-tree noise** — not load-time deps, never copy them. `date-tz.dll` / `libprotoc.dll` / `onnxruntime_providers_shared.dll` in `vcpkg_installed/.../bin/` are likewise outside the closure.

`FaceLoginCredentialProvider.dll` uses **static CRT (/MT)** and has **zero** non-system DLL deps, so it needs no runtime DLLs.

### A no-op `wails build` is usually wrong

If `enrollment_app/index.html`, `installer/FaceLoginSetup/app.go`, `frontend/src/App.vue`, or `internal/extract.go` changed, rebuild C++ (`index.html` is embedded into `FaceLoginConsole.exe`) **and** rerun `wails build` — the Wails step does not detect C++ artifact staleness in `resources/`.

## Installer Frontend (Vue 3)

```bash
cd installer/FaceLoginSetup/frontend
npm run dev      # Vite dev server
npm run build    # vue-tsc --noEmit && vite build
```

## Runtime Data & Dev Run

The service and enrollment app resolve `HKLM\SOFTWARE\FaceLogin\DataPath` at runtime (the installer sets DataPath to the install dir itself, so production resolves to `C:\Program Files\FaceLogin`). Both require `data\config.json`, `models\`, and `log\` directories under the resolved data dir. For a **dev run without the installer**, set DataPath or drop the EXE outside Program Files so the dev-mode trust set (EXE dir + ProgramData) kicks in — the commands below use ProgramData as the dev fallback:

```bash
# dev-only: ProgramData is a trusted fallback only when DataPath is empty / EXE is outside Program Files
dest="$PROGRAMDATA/FaceLogin"
mkdir -p "$dest/models" "$dest/data" "$dest/log"
cp installer/FaceLoginSetup/resources/models/*.onnx "$dest/models/"
# data/config.json is created with defaults if absent
```

Dev default config: `{ "match_threshold": 0.80, "anti_spoof_threshold": 0.28 }`. DirectShow is the only camera implementation. Worker preload is model/runtime-only; no configuration may open the camera before `AUTH_REQUEST`.

> `anti_spoof_threshold` (0.28 default; UI-adjustable 0.15–0.50 in 0.01 steps, calibration point 0.281) is the 50/50 dual-MiniFAS fusion threshold (`common/config_util.h`). `match_threshold` (0.80) is the 512-D Euclidean cutoff; `EmbeddingThresholdForDim` clamps it to [0.70, 1.00] (out-of-band snaps to 0.80). The 15-second auth timeout is a hardcoded constant (`FaceService::m_authTimeoutSeconds`).

**Run:** `scripts\start_standalone.bat` (stops service, registers DLL, starts in `-standalone` foreground mode), or directly `build\face_service\Release\FaceLoginService.exe -standalone`. Three modes: SCM service (default), `-install`, `-uninstall`, `-standalone`.

Startup prints harmless ONNX "Schema error: Trying to register schema..." noise. Real status is in `<DataPath>\log\service.log` and `<DataPath>\log\auth_worker.log` (production: `C:\Program Files\FaceLogin\log\`; dev/empty-DataPath fallback: `C:\ProgramData\FaceLogin\log\`). `Initialization complete` means the public pipe and worker lifecycle are up. At the sign-in screen or after locking, `Authentication worker ready in ... ms` means authentication is ready; the child logs its model load and exits after each terminal authentication result. If the dual-MiniFAS bundle cannot load, the worker startup fails closed and the service reports an authentication error until a later lock/auth request or `CONFIG_RELOAD` succeeds. End-to-end check: run `FaceLoginConsole.exe` as admin (enrollment GUI), then `regsvr32 build\credential_provider\Release\FaceLoginCredentialProvider.dll` so the lock-screen tile appears.

## 版本号与版本资源

**一键同步（2026-09-02，已验证）：** `powershell -ExecutionPolicy Bypass -File scripts/bump_version.ps1 <x.y.z[-suffix]>`（加 `-Dry` 预览）。同步下面全部位置（含仅磁盘的 AGENTS.md），每处替换计数校验、漏一处即报错；同版本号重跑写回字节无损。

发版时版本号共 **5 处**同步：根 `CMakeLists.txt` 的 `project(VERSION x.y.z)`、`installer/FaceLoginSetup/wails.json` Info、`installer/FaceLoginSetup/main.go` 的 `appVersion`、README badge、`installer/FaceLoginSetup/build/windows/info.json`（gitignored，`wails build` 实际读它，wails.json Info 不会同步过去）。AGENTS.md 概述里还有一处带后缀的显示版本（第 6 处，仅磁盘）。

版本**资源**（文件属性页可见）的生成机制（2026-09-02 修复）：

- CMake 产物（Service/Console/Provider）：根 `CMakeLists.txt` 的 `facelogin_add_version_rc()` + `version.rc.in` 模板，版本取 `project()` VERSION；新目标直接调用该函数，不要在各自 resource.rc 里手写 VERSIONINFO（provider 曾硬编码 1.4.0 引发双块冲突）。
- 安装器（wails）：`build/windows/info.json` 的 `"info"` 键是 **Windows 语言 ID**——必须是 `"0409"`（en-US）。模板默认的 `"0000"`（语言中性）会让 go-winres 产出 000004b0 块，.NET/资源管理器按 Translation 查不到 → 属性页全空（资源本体其实存在）。
- 校验：`PowerShell (Get-Item <exe>).VersionInfo.ProductVersion` 四个产物应均为 x.y.z。
