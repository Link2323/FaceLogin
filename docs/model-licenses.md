# FaceLogin 模型来源与许可

核对日期：2026-10-01。本文件随安装包以 `MODEL_LICENSES.md` 提供。

项目根目录的 MIT 许可适用于 FaceLogin 自有代码；第三方代码和预训练权重遵守各自的许可。模型下载、哈希验证、ONNX 转换或 INT8 量化均不构成新的用途授权。本文件记录来源及目前可见的许可依据，不替权利方作出授权。

## 当前授权状态

- **`face_recognition_sface_2021dec.onnx`：OpenCV 模型目录声明全部文件为 Apache-2.0。** 当前试用构建使用固定版本未修改的 FP32 权重，128 维。许可全文、模型目录说明和权重来源位于 `third_party/models/sface/`；按发布方条款保留声明，无需另行申请个别授权。训练数据权利未独立核验。R50 权重已退出安装载荷，其授权申请只作历史归档。
- **`det_10g_gnkps.onnx`：已补齐发布者及 Apache-2.0 许可来源。** 发布者为 Kun-Hsiang Lin（`kunkunlin1221`），其 PyFace 代码明确引用当前模型仓库，同作者的 DOCSAID FaceDetection 提供匹配的训练及导出实现。模型仓库对这份具体权重声明 Apache-2.0，相关训练源码也采用 Apache-2.0；发布版本、原始权重 SHA-256、许可原文及关联证据已固定。按发布者声明保留 Apache-2.0 全文、作者来源及 FaceLogin 修改说明。下面区分直接证据与训练归属推断；不能把架构同名当成 InsightFace 官方权重的证明。本次未独立核验训练数据的全部权利。
- **双 MiniFAS：按来源仓库的 Apache-2.0 声明保留许可。** yakhyo 的发布仓库及其引用的 MiniVision 上游仓库均提供 Apache-2.0；本次核对未见对这两份公开权重另列的用途限制。完整上游许可快照和来源链接收录在 `THIRD_PARTY_NOTICES.txt`。这不等于对训练数据权利作出独立保证。

当前四个交付模型按各发布方 Apache-2.0 声明分发，仍须遵守许可条款；本次不替训练数据权利作独立保证。SFace 的试用状态与安全标定进度见 `sface-trial.md`。

## 来源与处理

| 交付文件 | 基础模型来源 | FaceLogin 处理 | 可见许可依据 |
|---|---|---|---|
| `det_10g_gnkps.onnx` | [Kun-Hsiang Lin 发布的具体版本](https://huggingface.co/kunkunlin1221/face-detection_scrfd-10g-gnkps/tree/eb0e349519cd951b2a9423dac45b39e8ce5a71b8)，文件 `scrfd_10g_gnkps_fp32.onnx`；下载脚本使用同版本 hf-mirror.com 镜像 | FaceLogin 修改：移除输出 stride 缩放节点、调整输出顺序、静态 QDQ INT8 量化；保留上游元数据 | 发布者对该权重的 Apache-2.0 声明；相关 DOCSAID 训练源码及 Apache-2.0 全文见下文 |
| `face_recognition_sface_2021dec.onnx` | [OpenCV Zoo 固定版本](https://github.com/opencv/opencv_zoo/tree/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_recognition_sface) | 未修改 FP32 ONNX；输入 RGB 0..255，输出后 L2 归一化 | 模型目录所有文件 Apache-2.0；离线快照见 `third_party/models/sface/` |
| `MiniFASNetV2.onnx` | [yakhyo 的 weights 发布](https://github.com/yakhyo/face-anti-spoofing/releases/tag/weights)，同名 ONNX | 直接下载及哈希核验，无本地权重转换 | [yakhyo LICENSE](https://github.com/yakhyo/face-anti-spoofing/blob/main/LICENSE)、[MiniVision 上游 LICENSE](https://github.com/minivision-ai/Silent-Face-Anti-Spoofing/blob/master/LICENSE) |
| `MiniFASNetV1SE.onnx` | 同上，同名 ONNX | 直接下载及哈希核验，无本地权重转换 | 同上 |

上述文件分别提供人脸检测/五关键点、128 维 SFace 人脸识别、2.7× 裁剪活体检测和 4.0× 裁剪活体检测。FaceLogin 的原始下载与处理事实源为 `scripts/download_models.ps1`、`scripts/normalize_scrfd_export.py`、`scripts/download_minifas_models.ps1` 及 `tools/threshold_calibration/quantize_*.py`。

## 检测模型的来源核对

署名：Kun-Hsiang Lin（`kunkunlin1221`），DOCSAID / DocsaidLab。SCRFD 是采用的架构名称；FaceLogin 对发布文件进行了上述图规范化及量化修改。

**直接可核对的证据：**

1. [发布者的 Hugging Face 主页](https://huggingface.co/kunkunlin1221)直接链接同名 GitHub 账号。[固定版本模型卡](https://huggingface.co/kunkunlin1221/face-detection_scrfd-10g-gnkps/blob/eb0e349519cd951b2a9423dac45b39e8ce5a71b8/README.md)包含 `license: apache-2.0`，该声明针对模型仓库，而不是仅针对推理代码；正文未提供训练说明。
2. [PyFace 的固定版本 SCRFD 入口](https://github.com/DocsaidLab/PyFace/blob/96e85c050614d4d01297012ee535237378fe6b01/pyface/components/face_detection/scrfd.py)将 `scrfd_10g_gnkps_fp32` 明确映射到这个 Hugging Face 仓库及同名文件。
3. 同作者的 [FaceDetection 训练配置](https://github.com/DocsaidLab/FaceDetection/blob/b690d0b573f036cda224873e8997f03c9e5930ed/config/640-scrfd-10g-gnkps.yaml)标注 `phase: from-scratch`；头部为 80 通道、10 组 GroupNorm，输出为 `box_* / score_* / lmk5pt_*`。这些特征、输出 stride 缩放及元数据字段均与本地规范化 FP32 模型吻合。其 [LICENSE](https://github.com/DocsaidLab/FaceDetection/blob/b690d0b573f036cda224873e8997f03c9e5930ed/LICENSE)为 Apache-2.0。配置中的训练数据路径为 WIDERFace；评估还包含 NIST，评估数据名称不等于训练数据声明。
4. 本地 FP32 模型的导出信息是 PyTorch 2.4.1、日期 `2025-02-11 19:00:20`、206 个节点、130 个初始化张量。它与 SthPhoenix / InsightFace-REST 发布的同名 GNKPS 权重不是同一份导出：后者为 PyTorch 1.8、236 个节点、139 个初始化张量；元素数大于 16 的初始化张量按形状、类型和数据 SHA-256 比较，双方 117 / 108 个张量中完全相同者为 0。参考文件 MD5 与 [作者模型清单](https://github.com/SthPhoenix/InsightFace-REST/blob/fb854d8029c880dc5a244595e491c1293c218bff/models/models.json)的 `1d9b64bb0e6e18d4838872c9e7efd709` 相符。

**据此作出的归属判断：** 当前文件应关联 Kun-Hsiang Lin / DOCSAID 的训练及导出实现，而不是仅因 `scrfd_10g_gnkps` 同名就归属 InsightFace-REST 或 InsightFace 官方模型包。这是根据同一发布者、代码入口、图结构、配置及元数据形成的判断；没有训练日志或原始 checkpoint，张量比对也不能单独证明完整训练历史。用途许可依据是发布者对当前模型的 Apache-2.0 声明，训练数据权利不由该声明代为保证。

离线证据索引及比对记录、模型卡原文和 Apache-2.0 许可快照统一位于源码树 `third_party/models/scrfd-10g/`；活体模型的两份上游许可统一位于 `third_party/models/minifas/`。原文快照由 `third_party/sources.json` 记录来源、版本与 SHA-256，并随 `THIRD_PARTY_NOTICES.txt` 分发。重新下载原始检测文件时，脚本同时核验固定版本、字节数和原始 SHA-256，不再只检查文件大小。

## 历史 R50 授权申请（不再分发）

旧 R50 路线曾准备向 InsightFace 申请书面授权；该模型已退出当前交付载荷，以下资料仅为历史记录。[申请草稿](../third_party/models/w600k-r50/authorization-request.md)含可复制的英文邮件，发送前填写申请主体、回复邮箱、预计安装量及免费/商业分发计划。申请尚未发送，官方授权也尚未取得。

来源核对记录在 [`provenance.json`](../third_party/models/w600k-r50/provenance.json)：下载镜像的 LFS 文件大小与 SHA-256 匹配原始 FP32 固定值，当前 INT8 文件与安装器镜像一致。未在本次核对中下载 FP32 或与官方模型包逐字节比较，因此申请同时要求权利方确认这份哈希属于授权对象，必要时提供其认可的官方文件。

授权须明确覆盖日常登录/解锁、ONNX 与 INT8 衍生模型、公开安装包分发、终端用户使用和相应限制。仅收到询价回复、联系人或研究许可不能关闭此项。收到覆盖实际需求的书面授权后，保存证据并按授权条款更新声明；合同若保密，只公开允许披露的范围和证据引用。

## 当前交付文件标识

| 文件 | 字节数 | SHA-256 |
|---|---:|---|
| `det_10g_gnkps.onnx` | 4257451 | `07b62718eb454ee1881465c12d0d0546f2e916e3bb549f142dc221729bf7f4dc` |
| `face_recognition_sface_2021dec.onnx` | 38696353 | `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79` |
| `MiniFASNetV2.onnx` | 1743581 | `b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907` |
| `MiniFASNetV1SE.onnx` | 1742335 | `ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676` |

检测原始 FP32 发布文件为 16273449 字节，SHA-256 为 `2112d066c1dce6cc648670e69cf90561b9287bb1945153f3b461a487131255b9`；发布仓库版本为 `eb0e349519cd951b2a9423dac45b39e8ce5a71b8`。本地规范化 FP32 文件为 16272909 字节，SHA-256 为 `c940f97765fdc4b872b4a1ea041248d3e3d550202b7639f9488be558a6c0acb0`。

旧 R50 基础 FP32 文件的 SHA-256 为 `4c06341c33c2ca1f86781dab0e829f88ad5b64be9fba56e56bc9ebdefc619e43`（历史记录，不入包）。以上哈希用于识别具体文件，不代表权利方授权证明。

## 更新要求

模型或其许可改变时，同步本文件、下载/转换脚本及安装器/服务的模型清单。保存来自权利方的版本、来源、许可全文及授权范围；如签订了不宜公开的合同，公开文档应准确记录授权范围与适用模型，并保存可供审核的授权依据。
