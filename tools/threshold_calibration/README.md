# threshold_calibration

测量人脸嵌入距离分布，验证 `face_service/credential_store.h` 里 `EmbeddingThresholdForDim` 对 512-D 模型硬编码的 **0.80** 匹配阈值是否合理。

跑完会生成一份距离分布报告（same-person / other-person / photo 三类 + 阈值推荐），用来判断 0.80 是否安全、能否放开。本 README 讲**怎么用工具产出这份数据**。

## 这个工具测什么

复刻生产 C++ 认证管线的 Python 版：SCRFD 检测 → 5 点相似变换对齐 → w600k_r50 生成 512 维嵌入 → L2 归一化 → 欧氏距离。**每一步的归一化常数、解码公式、对齐模板都与 C++ 源码逐字段对齐**，所以测出的距离可以直接和生产阈值 0.80 比较。

它回答两个问题：

1. **陌生人能冒充你解锁吗？** → 测 other-person 距离（你和别人嵌入的最近距离）。实测都在 1.25 以上，离 0.80 极远，安全。
2. **你自己能稳定解锁吗？** → 测 same-person 距离（你的不同帧之间）。同摄像头条件下稳定在 0.40–0.78。

照片/屏幕翻拍攻击**不在这个工具的评估范围**——那是 PAD（双 MiniFAS 反欺诈层）的职责，用 `PadCalibration.exe` + `../../scripts/analyze_pad_calibration.py` 单独测。

## 安装依赖

```powershell
python -m pip install -r tools\threshold_calibration\requirements.txt
```

需要 `assets\models\` 下两个生产模型：`det_10g_gnkps.onnx`（SCRFD 检测器）和 `w600k_r50.onnx`（识别器）。模型路径不在默认位置时用 `--detector` / `--recognizer` 指定。

## 准备照片

按「一个文件夹 = 一个人」组织，每个人一个子目录：

```
my_faces/
  me/           ← 你本人，多张不同角度/表情
    001.jpg
    002.jpg
    ...
  mother/       ← 另一个人（家人/朋友）
    001.jpg
    ...
```

要点：
- 每个人至少 **5–10 张**，角度/光照多样，距离分布才有统计意义
- 至少 **2 个人**才能算 other-person 分布（单个人只能看 same-person）
- **人脸必须正立**——SCRFD 不认横躺的脸，详见下方「方向修正」

## 跑标定

最常见用法——用本地照片测 same-person 和 other-person 分布：

```powershell
python tools\threshold_calibration\calibrate.py `
    --images my_faces `
    --report docs\threshold-calibration.md
```

报告写到 `docs/threshold-calibration.md`，含 same-person / other-person 距离分布表 + 直方图 + 阈值推荐。脚本带 sanity check：若 same-person 分布没有任何一对落在 C++ baseline 区间 [0.40, 0.80]，会判定与 C++ 管线不一致而拒绝出报告。

### 方向修正（重要）

如果照片是横躺的（手机竖拍存成横向很常见），SCRFD 会自信地给出错误检测，距离会虚高。先探测正确方向：对一张照片试 4 个旋转，看哪个检测得分高且 bbox 接近正常脸的长宽比（宽/高 ≈ 0.6）。

```powershell
# 探测：见下方「常见问题」里的旋转探测脚本
# 确认是 90° 顺时针后，运行时旋转，不修改源文件：
python tools\threshold_calibration\calibrate.py `
    --images my_faces `
    --rotate-gallery 90 `
    --report docs\threshold-calibration.md
```

`--rotate-gallery` 对所有身份的图统一旋转。如果不同人的图方向不一致，先把各自的图转正再放进目录。

### 加照片攻击数据（可选，用于验证 PAD 必要性）

把屏幕显示的人脸照片用手机/显示器翻拍，放进单独目录（**不要放进 `--images` 的身份子目录**，否则会被当成另一个人混进 other-person 分布）：

```powershell
python tools\threshold_calibration\calibrate.py `
    --images my_faces\me `
    --photos attack_photos `
    --rotate-photos 90 `
    --report docs\threshold-calibration.md
```

`--images` 和 `--photos` 的旋转参数相互独立，因为它们的图通常来自不同设备、方向不同。

> 照片距离会很低（0.3–0.5），远低于 0.80。这是预期的——照片防护归 PAD，不归识别阈值。这组数据的意义是**证明 PAD 不可或缺**：一旦绕过 PAD，照片在识别层 100% 通过。

## 用公开数据集扩充 other-person 样本

本地能找到的「不同的人」通常有限。LFW（Labeled Faces in the Wild）提供几千个不同人的公开照片，下载器会取一个小子集（默认 30 人 × 8 张）：

```powershell
python tools\threshold_calibration\download_dataset.py
# 下载到 tools\threshold_calibration\data\lfw_subset\<身份>\<图>.jpg
python tools\threshold_calibration\calibrate.py `
    --images tools\threshold_calibration\data\lfw_subset `
    --report docs\threshold-calibration.md
```

下载器支持两个源（自动尝试，失败会明确报错并给出手动方案）：HuggingFace `bitmind/lfw` parquet（首选，经 hf-mirror.com）和 UMass 官方 tgz。首次下载约 180 MB，缓存在 `data\_cache\`，后续重跑免费。

## 常见问题

**某张图提示 "no face detected"**

正常——坏帧、过度模糊、严重遮挡会被跳过，脚本打印 `[warn]` 后继续。但如果**大量**图都检测不到，先怀疑方向问题（见上方「方向修正」）。

**旋转探测脚本**

```powershell
python -c "import sys; sys.path.insert(0,'tools/threshold_calibration'); from pathlib import Path; import cv2; from calibrate import ScrfdDetector, DEFAULT_MODELS_DIR, imread_unicode; det=ScrfdDetector(DEFAULT_MODELS_DIR/'det_10g_gnkps.onnx'); p=Path('my_faces\mother\001.jpg'); orig=imread_unicode(p); [print(f'{n:8s}', 'NO FACE' if (d:=det.detect_largest(img)) is None else f'score={d.score:.3f} bbox_w/h={(d.x2-d.x1)/(d.y2-d.y1):.2f}') for n,img in [('as-is',orig),('90 CW',cv2.rotate(orig,cv2.ROTATE_90_CLOCKWISE)),('180',cv2.rotate(orig,cv2.ROTATE_180)),('90 CCW',cv2.rotate(orig,cv2.ROTATE_90_COUNTERCLOCKWISE))]]"
```

哪个方向的 bbox_w/h 接近 **0.6**（而非 2 以上），那个就是正确旋转角度。

**LFW 下载失败（CDN reset / DNS）**

通常是网络问题，不是代码 bug。重试、挂代理，或按下载器失败提示手动放任意「按人分目录」的照片文件夹。

**报告里 same-person 分布出现双峰**

说明数据混合了不同拍摄条件（比如 webcam 实拍 + 证件照）。同条件帧之间距离正常（0.4–0.8），跨条件（证件照 vs webcam）会跳到 0.9–1.0。这是 w600k_r50 的固有特性，不是 bug。锁屏场景用同一摄像头，不触发跨条件问题。

## 文件清单

| 文件 | 作用 |
|---|---|
| `calibrate.py` | 标定主脚本，复刻 C++ 管线，生成距离分布报告 |
| `download_dataset.py` | LFW 公开数据集子集下载器 |
| `requirements.txt` | numpy / onnxruntime / opencv-python-headless |
| `data/` | 下载的数据集和缓存（gitignored） |
