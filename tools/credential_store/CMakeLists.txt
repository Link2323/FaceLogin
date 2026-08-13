# Development-only credential boundary and database compatibility test.
add_executable(CredentialStoreTest
    credential_store_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/credential_store.cpp
)

target_link_libraries(CredentialStoreTest PRIVATE
    facelogin_common
    shell32
)

target_include_directories(CredentialStoreTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(CredentialStoreTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
