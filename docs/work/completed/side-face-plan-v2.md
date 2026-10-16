# 侧脸识别方案 v2（多角度注册管线）

> 状态：**历史快照（v1.5 设计阶段）**。本文为 v1.5 侧脸方案的设计记录，正文冻结不再逐条维护——实现入口见 [`DEVELOPMENT.md`](../../../DEVELOPMENT.md)，当前认证管线见 [`docs/modules/face-service.md`](../../modules/face-service.md)，迁移前性能实验见 [`性能优化实验`](performance-baseline.md)，最终 worker 验收见 [`worker 迁移验收`](auth-worker-migration-completion.md)。已知与现状不符之处：① `w600k_r50` 已 INT8 量化为 ~44 MB（本文写 174 MB）；② dlib 已完全移除，图像容器/缩放/裁剪由 `common/frame_image.h` 自研（本文写"dlib 库仍保留"）；③ 检测器输入已改 512×512；④ 每认证嵌入已优化为 3 次（融合循环）。调研历史见文末附录。
> 渐进式学习的后续方案见 [`progressive-learning-v2.md`](../in-progress/progressive-learning-v2.md)；早期 R50 探讨见 [`progressive-learning.md`](progressive-learning.md)。

## 核心思路

纯软件侧脸识别的标准做法是**多角度注册 + 质量门控**（Windows Hello / FaceID 靠硬件 IR + 深度，本案靠软件）。

注册时存三把"钥匙"：

```
正对摄像头 0°      → 存正面模板 A
头向左转 30°       → 存左侧脸模板 B
头向右转 30°       → 存右侧脸模板 C
```

认证时比"谁最近就跟谁"——侧脸时的脸码离同角度模板近，离正面模板远。侧脸能被认出的原因在此。

## 管线五个环节（一次认证 = 连续 5 帧）

五个环节在一帧里串起来跑，连续 5 帧构成一次活体判定：

| 环节 | 做什么 | 模型 | 大小 |
|---|---|---|---|
| ① 检测 + 关键点 | 找脸 + 量角度 | **SCRFD 10g_gnkps**（gnkps = 组归一化关键点变体，修复旋转脸关键点不准） | ~16MB |
| ② 对齐 | 5 点相似变换摆正 | 无模型（纯几何，映射到 InsightFace 标准参考点） | 0MB |
| ③ 算脸码 | 人脸 → 512 维向量 | **w600k_r50**（IResNet-50 + ArcFace，L2 归一化） | 174MB |
| ④ 比对 | 判定是不是你 | 无模型（欧氏距离 + 账户级最近邻） | 0MB |
| ⑤ 活体 | 防照片/屏幕攻击 | **双 MiniFAS**（V2 2.7× + V1SE 4.0×）50/50 融合，强制 fail-closed | ~3.4MB |

活体的执行细节：两个 MiniFAS 模型**并发**评估（各自独立 session），算 real 概率后 50/50 算术平均得融合分；5 帧里**每一帧**的融合分都要过 `anti_spoof_threshold`（0.281），同时每帧都跑一遍嵌入 + 比对，要求连续 5 帧匹配到**同一个 SID**（防 PAD 过程中换脸）。最终放行条件：活体 5/5 全过 + 候选距离 < 0.80（512-D 标定）+ best/second-best ratio < 0.75，三者都满足才打包凭证返回；任一环失败直接 AUTH_ERROR。PAD 是 fail-closed 的——模型加载失败、SHA-256 校验失败、推理出错、帧间 SID 不一致，任一发生都拒绝认证，pipe 仍在线让锁屏收到 AUTH_ERROR 而不是超时卡死。

关键设计：
- SCRFD 的 5 个关键点直接用于对齐 → **整个环节不再需要独立关键点模型**（省掉旧管线 dlib 68 点的 99MB）
- 嵌入空间与旧模型不同 → 换模型必须**重新录入**
- 活体是静默的（不眨眼，侧脸时眼睛信息不全）；blink 通道随 dlib 一起删除

## 多角度注册流程

```
位置       目标偏航角   容差      每角度采样
正面       0°          ±10°      5 帧（每帧过 PAD）
左转       +30°        ±10°      5 帧
右转       −30°        ±10°      5 帧
```

- yaw 门控**严格 ±10°**（`EnrollmentWizard.cpp` 超容差即 `continue`，未实现规划中的"检测置信度+清晰度"回退）
- **跨角度绝不平均**——模型姿态敏感（同人正脸 vs 30° 侧脸距离 0.4–0.7，跨角度平均后易逼近阈值）
- 每角度独立 `AddFace`，三角度正好占满 V4 的 `kMaxFacesPerUser=3` 槽位

## 体积对比

| | 旧管线 (v1.4) | 当前 (v1.5) |
|---|---|---|
| 检测器 | det_500m ~3MB | **10g_gnkps ~16MB** |
| 关键点 | dlib 68 点 99MB | ✅ 删除（SCRFD 5 点代替） |
| 识别器 | w600k_mbf ~4MB | **w600k_r50 174MB** |
| 活体 | DeepPixBiS ~3MB | **双 MiniFAS ~3.4MB** |
| **合计** | **~109MB** | **~193MB** |

净增 ~84MB：识别器 +170MB 换来跨姿态能力翻倍，同时删除 99MB dlib。dlib **库**仍保留作图像容器/缩放依赖（删除的只是 99MB 的 68 点 `.dat` 模型和 face_detector/liveness_detector 模块）。

## 关键实现细节

### SCRFD 10g 导出归一化（`scripts/normalize_scrfd_export.py`）

10g 导出与 C++ 解码器有两处约定差异，必须用归一化脚本处理：
1. 输出张量按 stride 交错 → 重排为分组序（`score_8.., box_8.., kps_8..`）
2. box/lmk5pt 输出在图内预乘 stride（Mul 节点）→ 解码器会再乘一次导致关键点放远 stride 倍 → 剥掉 6 个 Mul 节点

不处理会导致对齐全废、匹配距离飙升到 1.25（陌生人水平）。

### 偏航角估计（`EstimateYawDeg`）

SCRFD 5 点几何估 yaw，弱透视模型 k=1.86（eyeDist/protrusion ≈ 65/35mm）。10g 关键点下 ±30° 读数吻合，无需校准。

### 阈值

512-D 嵌入的匹配阈值当前为 **0.80**（`EmbeddingThresholdForDim`），用户可在 [0.70, 1.00] 区间调（见 [`阈值标定记录`](threshold-calibration.md)）。

---

## 附录：调研历史（2026-08）

### 参考项目评估

| 项目 | 技术栈 | 结论 |
|---|---|---|
| AI Security System | MTCNN + FaceNet | ✅ 多角度注册是软件侧侧脸的正解，流程照抄 |
| rpi-face-recognition | SCRFD + MobileFaceNet | ⚠️ 与现有管线同构，无额外侧脸机制 |
| opencv (Haar+LBPH) | Haar + LBPH | ❌ 精度不足做认证 |
| dlib_face_recognition | dlib + ResNet | ⚠️ 借鉴 5 点对齐惯例 |

### 否决的路线

| 路线 | 结论 | 原因 |
|---|---|---|
| 正面化（GAN/3D 重建转正再比对） | ❌ | Pose-TTA 实测正面化降精度，引入形变和身份丢失；需存人脸图，与只存嵌入的架构冲突 |
| 3D 密集对齐（3DDFA-V2） | ⏸ | 增量收益在 >60°，不在 ±40° 范围 |
| dlib 68 点模型本身 | ❌ | 无法重训，用 SCRFD 5 点绕开 |
| MagFace 质量感知嵌入 | 💡 | 以后若换 MagFace，嵌入模长可免费做帧质量门控 |

### 与原方案的偏差

- 检测器：方案调研时为 34g，上线前降到 **10g** 档换取 ~3.4× 检测加速（WIDER Face −1%）
- 活体：未沿用 DeepPixBiS，改用**双 MiniFAS 50/50 融合**并改为强制 fail-closed
- 槽位上限：从规划的 5 降为 `kMaxFacesPerUser = 3`（多角度正/左/右正好占满）
- 迁移前性能优化详见 [`性能优化实验`](performance-baseline.md)；当前 worker 资源验收见 [`worker 迁移验收`](auth-worker-migration-completion.md)
