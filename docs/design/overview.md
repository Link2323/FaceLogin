# FaceLogin 设计概览

本文件覆盖**项目定位、技术栈、安全设计、账户兼容性**——给人建立全局理解用。任务/文件入口见 [`DEVELOPMENT.md`](../../DEVELOPMENT.md),协议契约见 [`docs/contracts/`](../contracts/);运行环境与排障见 [`docs/operations/operations.md`](../operations/operations.md)。

---

## 一、项目简介

FaceLogin 是一个 Windows 人脸识别登录系统，允许用户通过摄像头人脸识别解锁 Windows 桌面。项目基于 Windows Credential Provider 框架实现锁屏/登录界面集成，使用 ONNX Runtime 进行人脸检测（SCRFD）、识别（InsightFace w600k_r50）与活体检测（双 MiniFAS 融合）；图像容器/缩放/裁剪为自研 `common/frame_image.h`（v1.6 起 dlib 完全移除）。

---

## 二、技术栈

| 层面 | 技术 |
|---|---|
| 编程语言 | C++20, Go (安装程序) |
| 构建系统 | CMake 3.20+ |
| 包管理 | vcpkg |
| 人脸检测 | SCRFD 10g gnkps ONNX（dlib HOG 已移除） |
| 关键点 | SCRFD 自带 5 关键点（gnkps 变体），相似变换对齐（dlib 68 点 Shape Predictor 已移除） |
| 人脸识别 | InsightFace buffalo_l w600k_r50 ONNX（512 维，IResNet-50；dlib ResNet 已移除） |
| 活体检测 | MiniFASNetV2 + MiniFASNetV1SE 50/50 融合静默反欺诈 |
| 相机采集 | DirectShow |
| 凭据提供 | Windows Credential Provider COM (ICredentialProvider) |
| 进程通信 | 命名管道 (Named Pipe), UTF-16LE 编码 |
| 凭据加密 | DPAPI (CRYPTPROTECT_LOCAL_MACHINE) |
| 安装程序 | Go Wails v2 + Vue 3 |
| 控制台UI | WebView2 + HTML/CSS/JS (嵌入式资源) |
| 编译器 | MSVC 2022 (Visual Studio 2022) |

---

## 三、安全设计

| 层面 | 措施 |
|---|---|
| 进程通信 | 命名管道 DACL 限制 SYSTEM + Administrators |
| 凭据存储 | DPAPI 机器范围加密 (`CRYPTPROTECT_LOCAL_MACHINE`) |
| 管道安全 | `PIPE_REJECT_REMOTE_CLIENTS` 拒绝远程连接 |
| 单实例 | 全局互斥体防止多个服务实例 |
| DLL 安全 | `/DYNAMICBASE` (ASLR), `/NXCOMPAT` (DEP), `/GUARD:CF` (CFG), `/HIGHENTROPYVA` (64位) |
| 密码验证 | `LogonUserW(LOGON32_LOGON_NETWORK)` 轻量验证，不缓存凭据 |
| 活体检测 | MiniFASNetV2 + MiniFASNetV1SE 融合反欺诈，防止照片/视频攻击 |
| 匹配安全 | 欧氏距离阈值 + 最佳/次佳匹配比双重验证 |

---

## 四、账户兼容性

### 4.1 支持的账户类型

| 账户类型 | 登录 | 注册 | 说明 |
|---|---|---|---|
| 本地 SAM 账户 | ✅ | ✅ | `COMPUTERNAME\Username` 格式 |
| 微软在线账户 (MSA) | ✅ | ✅ | `user@outlook.com` UPN 格式 |
| 域账户 (Active Directory) | 理论支持 | 理论支持 | 使用 Kerberos 认证 |

### 4.2 MSA 实现细节

**身份获取**: `GetUserNameExW(NameUserPrincipal)` 在 MSA 关联机器上返回 `ERROR_NO_SUCH_USER` (1332)，因此增加了注册表回退方案：
- 读取 `HKLM\SOFTWARE\Microsoft\IdentityStore\LogonCache\D7F9888F-E3FC-49b0-9EA6-A85B5F392A4F\Name2Sid\{hash}` 中的 `IdentityName` 值
- 该值为 MSA 邮箱地址 (UPN 格式)

**凭据打包**: 本地账户使用 `Domain\Username` 格式，MSA 账户使用 UPN `user@domain.com` 格式。均使用 `MICROSOFT_AUTHENTICATION_PACKAGE_V1_0` 认证包。

**数据存储**: V4 数据库同时存储 username、UPN 和 SID，按 SID 优先匹配；每账号可存多张人脸。
