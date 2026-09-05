# `users.dat` 持久化契约

本文定义 FaceLogin 凭据数据库 V5 契约。实现事实源是 [`face_service/credential_store.h`](../../face_service/credential_store.h) 和 [`face_service/credential_store.cpp`](../../face_service/credential_store.cpp)。不要根据本文单独重写解析器；修改格式时必须同步所有消费者。

## 位置与安全属性

- 文件位置：`<DataPath>\data\users.dat`
- 默认生产位置：`C:\Program Files\FaceLogin\data\users.dat`
- 密码：DPAPI `CRYPTPROTECT_LOCAL_MACHINE` 加密，绑定本机
- `data\` 目录 DACL 仅 SYSTEM+Administrators（安装器 `SetDataDirectoryACL`）：机器作用域 DPAPI 密文任何本地账户都能解密，文件 ACL 是非管理员进程读取的唯一屏障；Users 对安装目录其余部分仍保持只读（DLL 加载所需）
- 文本：Windows `wchar_t[]`，UTF-16LE，不写结尾 `NUL`
- 数值：当前仅支持 Windows x64 本机产生的定长整数/IEEE 754 `float`

不得记录密码、DPAPI 密文或完整认证消息。密码解密后每条退出路径都要清零。

## V5 字节布局

所有长度字段均为元素个数；只有密码长度是字节数。

```text
[Header]
  magic:          uint32 = 0x474F4C46  // "FLOG"
  version:        uint32 = 5           // 读路径另接受 4（升级读）
  accountCount:   uint32

[Account] × accountCount
  usernameLen:    uint32
  username:       wchar_t[usernameLen]
  upnLen:         uint32
  upn:            wchar_t[upnLen]
  sidLen:         uint32
  sid:            wchar_t[sidLen]
  passwordLen:    uint32               // bytes
  passwordBlob:   uint8_t[passwordLen] // DPAPI blob (length >= 2)
  faceCount:      uint32

  [Face] × faceCount
    faceId:       uint32
    labelLen:     uint32
    label:        wchar_t[labelLen]
    embeddingLen: uint32               // float count
    embedding:    float[embeddingLen]
    nominalYaw:   float                // V5：角度槽标称 yaw（度）       ← V5
    nominalPitch: float                // V5：角度槽标称 pitch（度）     ← V5
```

`nominalYaw`/`nominalPitch` 是录入时的**姿态目标**（`EnrollmentWizard` 的 `kAngleTargets`：正面 0/0、左转 +30/0、右转 −30/0），不是实测均值——固定目标保证三个角度槽的 ±25° 学习锥（[`docs/progressive-learning-v2.md`](../progressive-learning-v2.md) §3）互不重叠，即使用户转角不足。合法值域 ±90°；无效哨兵 `1000.0f` 表示"无角度信息"，V4 存量记录与未传角度的内存构造均携带它，序列化按原值往返（锥门对其放行）。

当前写入上限：

- `kMaxUsers = 5`
- `kMaxFacesPerUser = 3`
- `faceId >= 1`，账号内唯一
- 新增脸复用最小空闲 ID
- 当前正式 embedding 为 512 维

读路径对 `faceCount` 接受 1..16，embedding 长度接受 64..4096。不要把读路径宽容度误认为新数据写入上限。

## 版本策略

读路径接受 V4 与 V5，写路径只写 V5。V4 文件按无标称角（哨兵）读入，下次保存即升级为 V5——存量安装无需重新录入。V3 及更早、V6 及更新一律拒绝（fail-closed）。Credential Provider 只读 header（magic/version/count）判断磁贴，接受 4 与 5。

## 账号与人脸操作语义

- `AddFace`：账号不存在则创建；存在则追加人脸，不覆盖已存密码。
- `DeleteFace`：删除指定脸；若账号不再有脸则移除整个账号。
- `ClearFacesForAccount`：清空该账号人脸但保留身份/密码，供多角度重录替换。
- `ClearAllFaces` / `DeleteUserBySid`：删除整个账号。
- `UpdateAccountIdentity`：更新身份与密码，保留全部人脸。
- `RenameFace`：只改标签。
- 所有写操作都必须显式调用 `SaveDatabase()` 才落盘。

多角度注册每个角度独立保存一条 embedding 并写入该角度的标称 yaw/pitch；禁止跨角度平均。

## 匹配语义

1. 只比较相同 embedding 维度。
2. 账号内取所有脸的最小距离作为账号距离。
3. 账号间选 best / second-best。
4. best 必须小于维度对应阈值。
5. 多账号时执行最佳/次佳比门控；单账号不执行 ratio 拒绝。
6. 成功后才解密密码；使用后立即清零。

512 维阈值由 `EmbeddingThresholdForDim` 再次收口。具体数值和标定依据见 [`docs/threshold-calibration.md`](../threshold-calibration.md) 与 [`docs/modules/face-service.md`](../modules/face-service.md)。

## 消费者与兼容风险

- `FaceLoginService`：完整加载、匹配、保存
- `FaceLoginConsole`：注册、追加、删除、重命名、身份更新
- Credential Provider：只读 header（接受 V4/V5）判断是否显示磁贴

## 修改检查表

格式或语义变化时至少同步检查：

- `face_service/credential_store.h/cpp`
- `credential_provider/FaceLoginProvider.cpp` 的 header 读取
- `enrollment_app/EnrollmentWizard.*`
- 安装/升级/卸载是否保留或清除用户数据
- 本文和 `DEVELOPMENT.md`

验证至少覆盖：空数据库、不支持的版本拒绝、V4 升级读与 V5 标称角往返、多脸、密码 blob 长度、上限、截断/异常长度、删除最后一张脸、保存后重载、不同 embedding 维度，以及 DPAPI 解密失败。
