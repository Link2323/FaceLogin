add_executable(FaceGainTuneTest face_gain_tune_test.cpp)
target_include_directories(FaceGainTuneTest PRIVATE ${CMAKE_SOURCE_DIR}/face_service)
target_link_libraries(FaceGainTuneTest PRIVATE facelogin_common onnxruntime::onnxruntime)
set_target_properties(FaceGainTuneTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
