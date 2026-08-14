add_executable(ModelIntegrityTest
    model_integrity_test.cpp
)

target_link_libraries(ModelIntegrityTest PRIVATE facelogin_common)

target_include_directories(ModelIntegrityTest PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/face_service
)
