#pragma once
// model_failure.h — category-level reasons and lock-screen copy for a failed
// authentication model bundle load. Shared by the standalone in-process
// loader and the one-shot auth worker so both surfaces produce the same
// fail-closed message. Category-level only: the copy must tell the user what
// to do without leaking file names or hash detail.

namespace facelogin {

enum class ModelLoadFailure {
    None = 0,
    DetectorIntegrity,
    RecognizerIntegrity,
    PadIntegrity,
    Load,
};

// The worker sends this text as its Fatal payload and the parent relays it
// verbatim over the public pipe; standalone picks the same copy directly.
// `None` is not a real outcome — callers that lost the reason fall back to
// the generic load message, never to success.
inline const wchar_t* ModelLoadFailureMessage(ModelLoadFailure failure) {
    switch (failure) {
    case ModelLoadFailure::DetectorIntegrity:
        return L"检测模型完整性校验失败，文件可能被篡改或损坏，请使用密码登录并重新安装 FaceLogin";
    case ModelLoadFailure::RecognizerIntegrity:
        return L"识别模型完整性校验失败，文件可能被篡改或损坏，请使用密码登录并重新安装 FaceLogin";
    case ModelLoadFailure::PadIntegrity:
        return L"活体模型完整性校验失败，文件可能被篡改或损坏，请使用密码登录并重新安装 FaceLogin";
    case ModelLoadFailure::Load:
    case ModelLoadFailure::None:
        break;
    }
    return L"认证模型加载失败，请使用密码登录并检查模型文件";
}

} // namespace facelogin
