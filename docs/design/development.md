# FaceLogin 开发指南

本文件覆盖**本地开发流程**与**编码规范**。构建命令与产物路径见 [`docs/BUILD.md`](../BUILD.md);任务/文件入口见 [`DEVELOPMENT.md`](../../DEVELOPMENT.md),线格式与磁盘格式见 [`docs/contracts/`](../contracts/)。

---

## 一、本地开发模式

```cmd
REM 1. 下载模型
powershell -File scripts\download_models.ps1

REM 1.1 为本机服务运行时准备模型 (dev-only: 开发态 EXE 在 Program Files 外,
REM      ProgramData\FaceLogin 在 ResolveSecureDataDir 的信任集内; 生产部署数据
REM      在安装目录 C:\Program Files\FaceLogin\, 由安装器写入 DataPath)
mkdir C:\ProgramData\FaceLogin\models
copy assets\models\*.onnx C:\ProgramData\FaceLogin\models\

REM 2. 停止已有服务
sc stop FaceLoginService

REM 3. 以 standalone 模式运行服务 (前台 + Debug 输出)
FaceLoginService.exe -standalone

REM 4. 部署 DLL 并注册
regsvr32 build\credential_provider\Release\FaceLoginCredentialProvider.dll

REM 5. Win+L 锁屏测试
```

---

## 二、编码规范

- C++20 标准, `/W4 /WX-` 警告级别
- CRITICAL_SECTION 用于线程同步
- `FACELOGIN_*` 宏用于日志
- 中文字符串需要 MSVC `/utf-8` 编译选项
- 错误处理: 返回 `bool`，通过日志记录详细错误
- Go 代码遵循标准 Go 风格
