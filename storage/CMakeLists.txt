add_library(facelogin_credential_store STATIC
    credential_store.cpp
    credential_store.h
)
target_include_directories(facelogin_credential_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(facelogin_credential_store
    PUBLIC facelogin_common
    PRIVATE shell32
)
