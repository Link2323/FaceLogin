#pragma once

// SHA-256 values for the four production ONNX models. The service and its
// short-lived authentication worker must validate exactly the same payloads;
// keeping the C++ values here prevents lifecycle refactors from silently
// creating two divergent security boundaries.
namespace facelogin::model_hashes {

inline constexpr char kDetector[] =
    "07b62718eb454ee1881465c12d0d0546f2e916e3bb549f142dc221729bf7f4dc";
inline constexpr char kRecognizer[] =
    "ae6a6ac44d2bdc87924e75fb23d8212430dd24f037f5e035c21deff99afc8b61";
inline constexpr char kMiniFasV2[] =
    "b32929adc2d9c34b9486f8c4c7bc97c1b69bc0ea9befefc380e4faae4e463907";
inline constexpr char kMiniFasV1Se[] =
    "ebab7f90c7833fbccd46d3a555410e78d969db5438e169b6524be444862b3676";

} // namespace facelogin::model_hashes
