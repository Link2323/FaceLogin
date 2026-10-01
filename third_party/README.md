> 当前识别权重已切换为 OpenCV SFace，许可与固定来源在 `models/sface/`。`models/w600k-r50/` 为历史材料，索引标记 distributed=false，不再打入声明或模型载荷。

# Third-party notice sources

Files are grouped by component so each model's license and provenance can be
read together:

```text
third_party/
  sources.json                 # Snapshot origins, revisions and SHA-256
  models/
    scrfd-10g/
      LICENSE.txt              # Related DOCSAID training-source license
      model-card.md            # Publisher's weight-license declaration
      provenance.json          # Source evidence and model comparison
    minifas/
      yakhyo-LICENSE.txt        # ONNX download/export repository
      minivision-LICENSE.txt   # Cited training-source upstream
    w600k-r50/
      model-zoo.md             # InsightFace's pretrained model policy
      provenance.json          # Mirror revision/hash and pending authorization
      authorization-request.md # Unsent licensing inquiry draft
  libraries/
    onnxruntime/
      ThirdPartyNotices.txt     # ONNX Runtime 1.23.2 upstream notices
```

This tree contains legal/source records. Runtime ONNX files remain in
`assets/models/` and the installer payload mirror.

`sources.json` records the origin, revision and SHA-256 of the unmodified
notice/license snapshots. ONNX Runtime's notice snapshot corresponds to the
installed 1.23.2 release. The two MiniFAS license snapshots come from immutable
revisions of the download/export repository and its cited upstream project.
The DOCSAID SCRFD model card snapshot records the publisher's Apache-2.0
declaration for the pinned model revision. The related FaceDetection source
license is included separately; code licensing is not used to invent a weight
grant. `models/scrfd-10g/provenance.json` distinguishes direct source evidence,
graph observations and inferred training attribution.
The recognizer's `models/w600k-r50/model-zoo.md` preserves its official
non-commercial research policy. Its provenance record pins the download mirror
and distinguishes file identity from permission. The authorization inquiry
draft is source-only documentation and is not an authorization grant.

`../THIRD_PARTY_NOTICES.txt` is the checked-in distribution bundle. Regenerate it
after dependency upgrades with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/update_third_party_notices.ps1
```

Generation reads the actual CMake vcpkg install tree, Windows production Go
dependencies (including nested package licenses), the Go toolchain license,
the WebView2 SDK license/notices, and installed frontend package licenses.
Dependencies must already be restored. An ONNX Runtime version change requires
a matching upstream notice snapshot; generation never downloads legal texts or
models. Original texts are retained in English without translation.

`stage_licenses.ps1` copies the canonical project license, notice bundle and
[`MODEL_LICENSES.md`](MODEL_LICENSES.md) into the installer payload and frontend public assets.
The frontend's `prebuild` hook embeds these documents in both full and slim
Wails executables. They can be read in the installer before installation and
are also deployed as text files beside the installed programs.

The user-facing model license summary is [`MODEL_LICENSES.md`](MODEL_LICENSES.md).
Detailed model provenance and permission research remain in
[`model-licenses.md`](../docs/model-licenses.md); supplying notices does not
resolve rights outside the licenses stated by the publishers. Eigen's
version-specific source link is in the notice bundle to make its MPL-covered
source available to recipients. If Eigen is modified,
the corresponding modified sources must also be made available.
