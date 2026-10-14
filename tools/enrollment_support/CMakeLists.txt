add_executable(EnrollmentSupportTest enrollment_support_test.cpp)
target_link_libraries(EnrollmentSupportTest PRIVATE facelogin_enrollment_support)
set_target_properties(EnrollmentSupportTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
