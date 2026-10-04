> 2026-10-01：当前构建为 SFace 试用版（128 维、`users.dat` V6），强制重新录入并关闭渐进学习。匹配阈值 1.00 尚未完成生产安全标定；试用状态见 [SFace 记录](../sface-trial.md)。

# 人脸认证服务

本文说明 `face_service/` 的进程边界和认证约束。实现入口为 [`FaceService.cpp`](../../face_service/FaceService.cpp)。公共 IPC、worker 私有 IPC 和数据库格式分别以 [`IPC 契约`](../contracts/ipc.md)、[`worker IPC 契约`](../contracts/auth-worker-ipc.md) 和 [`users.dat 契约`](../contracts/users-dat.md) 为准。

## 职责与生命周期

服务父进程负责 DataPath、配置、`users.dat`、公开命名管道和身份匹配。锁屏期间异步预载一个 `-auth-worker`，worker 持有 ONNX 模型并在认证请求后打开摄像头，完成一次请求即退出。父进程不持有 ONNX Session，也不会在认证前打开摄像头。

worker 预载诊断在 `auth_worker.log` 输出当前 Session 与显式电源限流标志、三个模型组各自的 SHA-256 校验/初始化耗时，以及模型加预热、相机图骨架和总预载耗时。电源标志不等于实际 CPU 频率，也不足以排除自动调度限制；桌面探针不能替代锁屏 Session 0 测量。桌面加载实验见 [`sface-performance.md`](../sface-performance.md)。

ONNX Runtime 的 Warning/Error 通过自定义 Env 日志回调写入宿主的项目日志，使用原有分级刷盘；回调不向 ORT 的 C 接口抛出异常。保留默认 Warning 级别及原有模型加载顺序，避免无控制台 Session 0 中默认 `std::wclog` 输出让 SFace 初始化多付约 1 秒。性能对照与输出一致性证据见 [`sface-performance.md`](../sface-performance.md)。

```text
Initialize → 校验路径/加载配置和数据库/启动管道与生命周期线程
锁屏       → 异步预载 worker 与模型
AUTH_REQUEST → worker 打开相机并认证 → 父进程匹配 SID、构造认证结果
解锁/登录   → 关闭未使用的 worker
```

认证成功、失败、超时或通信异常后，worker 都会退出；服务在锁屏期间按需补建。模型不完整或加载失败时拒绝认证。父进程通过私有管道接收最多三条 SFace embedding，密码解密及 `AUTH_SUCCESS` 构造留在父进程。worker 生命周期和消息字段见私有 IPC 契约。

生产服务路径必须解析到 EXE 所在安装目录，不可信重定向会被拒绝。standalone 的开发路径规则见 [`DEVELOPMENT.md` 路径表](../../DEVELOPMENT.md#sec-paths)。

## 认证流水线

`AuthPipeline` 是 worker 与 standalone 共用的认证实现。主要约束如下：

1. 没有已注册账户时直接失败；双 MiniFAS 活体检测始终启用，任何模型错误都按失败处理。
2. 摄像头启动后先做曝光预热。人脸亮度出带时，按设备能力调节曝光和增益；有增益时优先用增益细调，无增益时优先保持或恢复驱动自动曝光。指定 `DevicePath` 时必须使用该设备，找不到即失败，不得回退到其他相机。
3. 调节器须等待可观察的亮度响应与稳定；没有响应证据至少等待 900 ms，持续漂移或未稳定时不确认结果。稳定调节结果可写入 DataPath 根目录的 `camera_tune.state`；取消、丢脸或未稳定不得覆盖旧记录。亮度余量允许时回收过长曝光，但不能低于人脸亮度底线。
4. 无脸且画面大面积饱和时可以有限下调曝光以恢复检测，仍须等待驱动稳定；该恢复不保存参数。注册预览使用同一调节逻辑，并在采集期间跳过后台调节。
5. 预环后若首个计帧前检测到迟到入场或亮度漂移，可纠正曝光，最多两次并丢弃触发帧重抓。计帧开始后不再修改传感器参数；纠正期间暂停并平移 PAD 计时，全局 15 秒时限继续计时。
6. 每个计入的帧都必须通过双 MiniFAS PAD。默认 5 帧，首、中、末绑定帧必须匹配同一 SID；非绑定帧以 bbox IoU 保持连续性。`fast_unlock` 模式使用 3 帧且每帧绑定。相邻计帧采集至少间隔 60 ms，帧序号必须递增。
7. 最佳身份须同时通过距离阈值和最佳/次佳比门控。成功后才生成认证结果；失败时清除敏感数据。

预热与传感器控制实现见 `exposure_warmup.h`、`face_gain_tune.h` 和 `auth_pipeline.cpp`。曝光探针的使用方法见 [`BUILD.md`](../BUILD.md)。

## 摄像头与模型

SFace 正式运行导出已移除 174 个 initializer graph inputs，保留权重、节点和真正的图像输入；因此不再产生这组 ORT WARN。运行哈希由 `common/model_hashes.h` 固定，与原始上游哈希不同；来源及转换记录见 `third_party/models/sface/provenance.json`。已有 V6/SFC1 人脸库保持兼容，本次不改识别阈值或要求重录。原生输出与旧库对照、初始化/推理取舍见 [`sface-performance.md`](../sface-performance.md)。

所有运行期相机路径使用 DirectShow。锁屏预载只枚举设备并准备图骨架，不激活设备；设备仅在认证请求或用户启动预览后激活。worker 在 MTA，Enrollment 预览在 UI STA；DirectShow COM 初始化和释放必须留在同一线程。改动相机生命周期时要覆盖 Session 0 与桌面预览。

| 模型 | 输入/输出 | 用途 |
|---|---|---|
| `det_10g_gnkps.onnx` | 512×512，框与 5 个关键点 | SCRFD 检测、相似变换对齐 |
| `face_recognition_sface_2021dec.onnx` | 112×112，128 维 embedding | SFace 身份识别 |
| `MiniFASNetV2.onnx`、`MiniFASNetV1SE.onnx` | 2.7× / 4.0× 人脸裁剪 | 双模型融合 PAD |

worker 和 standalone 在创建 Session 前校验四个模型的 SHA-256；C++ 哈希见 `common/model_hashes.h`，安装器提取时也会校验。普通构建复用现有模型，不要重新下载或量化。

worker 的模型状态为 `Unloaded → Loading → Ready/Failed`。加载、释放由生命周期线程串行处理；配置重载会替换锁屏 worker，并在新 worker 就绪后确认。服务与 worker 分别写 `service.log`、`auth_worker.log`；排障步骤见 [`运维手册`](../operations/operations.md)。

## 当前关键值

| 项目 | 当前值 |
|---|---|
| SFace 匹配阈值 | 默认 1.00，试用范围 [0.70, 1.00]；非生产标定 |
| 最佳/次佳比门控 | 0.75 |
| PAD 融合与阈值 | 双 MiniFAS 各 0.5；默认 0.28（标定点 0.281） |
| PAD 计帧 | 默认 5；`fast_unlock` 为 3 |
| 全局认证时限 | 15 秒；PAD 独立窗口 8 秒 |
| 无脸 / 持续 PAD 拒绝快速失败 | 2.5 秒 / 2 秒 |

阈值的标定证据见 [`threshold-calibration.md`](../threshold-calibration.md)；SFace 试用限制见 [`sface-trial.md`](../sface-trial.md)。

## 故障处理与修改检查

- 模型缺失、哈希不符、PAD 推理异常或 worker 通信失败：拒绝人脸认证，保留 Windows 密码登录。
- DataPath 不可信：服务拒绝启动。
- 无人脸、PAD 失败或身份不一致：返回认证错误；全局时限耗尽：返回超时。
- 修改认证顺序时检查 IPC、凭据匹配和密码清零；修改模型时同步哈希、安装器校验和标定文档；修改相机时覆盖 child 生命周期、Session 0 和桌面预览。

最低验证命令见 [`BUILD.md`](../BUILD.md)。相机、Credential Provider 和锁屏行为还需按实际设备验收；自动化测试不能替代实机认证。
