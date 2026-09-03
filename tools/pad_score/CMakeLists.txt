# Offline PAD scorer for still images (not part of the shipped product;
# built on demand with
#   cmake --build build --config Release --target PadScore)
add_executable(PadScore
    pad_score.cpp
    ${CMAKE_SOURCE_DIR}/face_service/onnx_models.cpp
)

target_link_libraries(PadScore PRIVATE
    facelogin_common

    onnxruntime::onnxruntime
)

target_include_directories(PadScore PRIVATE ${CMAKE_SOURCE_DIR}/face_service)

set_target_properties(PadScore PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
