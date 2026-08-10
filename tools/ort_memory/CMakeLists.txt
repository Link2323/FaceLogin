# Development-only ORT memory high-water probe. This target is never shipped.
add_executable(OrtMemoryTest
    ort_memory.cpp
    ${CMAKE_SOURCE_DIR}/face_service/onnx_models.cpp
)

target_link_libraries(OrtMemoryTest PRIVATE
    facelogin_common
    onnxruntime::onnxruntime
)

target_include_directories(OrtMemoryTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(OrtMemoryTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
