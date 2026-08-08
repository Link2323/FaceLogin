# One-off verification tool for the INT8 r50 experiment (not part of the
# shipped product; built on demand with
#   cmake --build build --config Release --target EmbeddingTest)
add_executable(EmbeddingTest
    embedding_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/onnx_models.cpp
)

target_link_libraries(EmbeddingTest PRIVATE
    facelogin_common

    onnxruntime::onnxruntime
)

target_include_directories(EmbeddingTest PRIVATE ${CMAKE_SOURCE_DIR}/face_service)

set_target_properties(EmbeddingTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
