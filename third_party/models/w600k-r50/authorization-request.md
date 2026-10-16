# FaceLogin 识别模型授权申请

状态：申请草稿，尚未发送，尚未取得书面授权。核对日期：2026-09-30。

保留现有识别模型，向权利方申请覆盖日常认证及安装包再分发的许可。公开的非商业研究政策不能代替这份授权；现有 INT8 模型暂不替换。模型来源、固定版本及哈希见同目录 `provenance.json`，官方政策原文见 `model-zoo.md`。

官方识别模型授权邮箱：`recognition-oss-pack@insightface.ai`，来自 [InsightFace 固定版本 README](https://github.com/deepinsight/insightface/blob/317d33497a4342351666a1dec69b7176eb67084e/README.md#license)。官方另有 [模型授权页面](https://www.insightface.ai/solutions/face-recognition-licensing)。

发送前填写申请人姓名或主体、回复邮箱、预计安装量，以及软件是否收费或用于商业部署。仓库的代码许可证和作者署名不能代替合同中的被许可人身份。下文询问免费开源发布是否可获免费授权；不预先承诺付费或部署规模。

## 可复制的英文邮件

To: recognition-oss-pack@insightface.ai

Subject: Licensing inquiry: w600k_r50 ONNX / INT8 redistribution in FaceLogin

Hello InsightFace licensing team,

I maintain FaceLogin, an open-source Windows x64 desktop application whose own code is MIT-licensed: https://github.com/EthanZer0/FaceLogin . We are seeking written permission for the face recognition model before treating it as approved for ordinary product use or redistribution.

FaceLogin uses an enrolled face to authenticate a Windows account for login/unlock through a local Windows service and Credential Provider. Inference runs offline on the user's CPU with ONNX Runtime; face images and embeddings are not sent to a server. This is an authentication use case, not solely academic research.

Our current recognizer is named w600k_r50.onnx, understood to be the ResNet50/WebFace600K model family. The FP32 file was obtained from the third-party mirror https://huggingface.co/richarrrddd/w600k_r50_v1 at revision 6c1850698e6b40a2c24f353531545bca5c5864ff:

- FP32: 174383860 bytes; SHA-256 4c06341c33c2ca1f86781dab0e829f88ad5b64be9fba56e56bc9ebdefc619e43.
- FaceLogin derivative: static per-tensor QDQ INT8 ONNX, 43805153 bytes; SHA-256 b9b2ea32afaa88dfd226255f354ea241c3a744abf75b3dbdcf00c95f7f00e185.

Please confirm whether this specific source artifact is covered by your licensing offer. If necessary, please identify the official artifact and hash that should be used instead.

We request clarification and permission covering:

1. Ordinary Windows login/unlock authentication by recipients, including non-research use and any permitted commercial deployment.
2. ONNX conversion and INT8 quantization, and use of the resulting derivative weights.
3. Embedding and redistributing the approved weights and derivative in publicly downloadable installers, and installing them onto recipients' computers.
4. Whether recipients of a freely distributed open-source application may use the bundled model under our license, or must obtain individual licenses.
5. Required notices, permitted public sharing of model/derivative files, deployment limits, license term, fees and renewal conditions. The model would remain under its own terms, separate from our code's MIT license.

Is a no-fee authorization available for free open-source distribution? If not, please provide the applicable licensing options. We are requesting information and permission, not committing to a purchase.

Applicant/licensee: [Your legal name or legal entity]
Reply email: [Your email]
Distribution and commercial-use plans: [Free/paid; personal/enterprise uses]
Estimated deployments: [Estimate or unknown; worldwide public downloads if applicable]

Thank you,
[Your name]
FaceLogin maintainer

## 收到回复后如何收尾

确认书面授权针对实际申请主体和上述权重，明确日常认证、量化衍生版本、软件再分发及终端用户使用范围；记录期限、数量、费用和署名要求。若只提供另一份模型的授权，先核对文件与适配影响，不能直接把授权套到当前哈希。

保存邮件或合同及可核对的授权条款，再更新 `provenance.json` 的授权状态和 `docs/model-licenses.md`。如合同保密，源码树仅记录可公开的授权范围及证据引用，原件另行保存。未获得覆盖所需范围的正式授权前，保留“授权待解决”状态。
