# FaceLogin 随附模型许可

FaceLogin 使用以下预训练模型进行人脸检测、人脸识别和活体检测。模型由各自的发布方提供，按其发布时声明的 Apache License 2.0 随 FaceLogin 分发。请同时阅读安装包中的 `THIRD_PARTY_NOTICES.txt`，其中收录了相关许可文本和来源声明。

| 模型 | 用途 | 来源与说明 |
|---|---|---|
| SCRFD 10G GNKPS | 人脸检测和五点关键点定位 | Kun-Hsiang Lin / DOCSAID 发布的权重。FaceLogin 对模型进行了图结构调整和 INT8 量化。 |
| OpenCV SFace 2021dec | 人脸特征提取 | OpenCV Zoo 固定版本权重，FaceLogin 使用未修改的 FP32 模型。 |
| MiniFASNetV2 | 活体检测 | yakhyo 发布的 ONNX 权重。 |
| MiniFASNetV1SE | 活体检测 | yakhyo 发布的 ONNX 权重。 |

FaceLogin 自有代码适用项目根目录所附的 MIT 许可证；该许可证不替代第三方模型各自的许可。模型许可和来源声明也不构成对训练数据权利的额外保证。
