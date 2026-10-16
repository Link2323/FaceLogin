# Development-only protocol and validation test. Never shipped.
add_executable(AuthWorkerProtocolTest
    auth_worker_protocol_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/auth_worker_protocol.cpp
)

target_link_libraries(AuthWorkerProtocolTest PRIVATE
    facelogin_common
)

target_include_directories(AuthWorkerProtocolTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)
