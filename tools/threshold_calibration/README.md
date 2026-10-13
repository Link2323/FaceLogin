# SCRFD 模型准备

此目录提供 SCRFD 静态 QDQ INT8 量化及所需的数据准备。当前四个正式模型的来源、哈希与转换流程见 [模型许可与来源](../../docs/model-licenses.md)，生产模型准备入口为 `scripts/download_models.ps1`。

普通构建复用 `assets/models/` 中已验证的模型，不运行下载或量化。

## 依赖

```powershell
python -m pip install -r tools/threshold_calibration/requirements.txt
```

## 标定图像

`quantize_scrfd.py` 默认读取 `data/lfw_subset/`，按每个身份一个子目录组织真实照片。可用 `--images` 指定其他图像目录。

新克隆缺少标定图像时，可从仓库根目录运行：

```powershell
python tools/threshold_calibration/download_dataset.py
```

下载的数据集和缓存均位于 Git 忽略的 `data/`。

## 量化入口

```powershell
python tools/threshold_calibration/quantize_scrfd.py --help
```

正式准备流程由 `scripts/download_models.ps1` 校验上游、规范化检测器、调用量化并核验最终文件大小和 SHA-256。量化脚本从 `calibrate.py` 复用检测、图像读取和预处理实现；`calibrate.py` 的 R50 识别标定命令不适用于当前 SFace 阈值。

PAD 实测使用 `tools/pad_calibration/` 和 `scripts/analyze_pad_calibration.py`。当前 SFace 试用范围与验收见 [试用记录](../../docs/work/in-progress/sface-trial.md)。
