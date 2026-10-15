# Execute the production state machine with a deterministic worker double.
# No camera, models, installed service, child processes or private pipes.
add_executable(AuthenticationResidencyTest
    authentication_residency_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/authentication_residency.cpp
)
target_link_libraries(AuthenticationResidencyTest PRIVATE facelogin_common onnxruntime::onnxruntime)
target_include_directories(AuthenticationResidencyTest PRIVATE ${CMAKE_SOURCE_DIR}/face_service)
