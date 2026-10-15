# Development-only credential boundary and database compatibility test.
add_executable(CredentialStoreTest
    credential_store_test.cpp
)

target_link_libraries(CredentialStoreTest PRIVATE facelogin_credential_store)
