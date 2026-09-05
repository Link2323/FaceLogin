# Development-only learner test: pose-based update-target selection and the
# gate chain (docs/progressive-learning-v2.md §3, 2026-09-05 revision).
add_executable(TemplateLearnerTest
    template_learner_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/template_learner.cpp
)

target_link_libraries(TemplateLearnerTest PRIVATE
    facelogin_common
)

target_include_directories(TemplateLearnerTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(TemplateLearnerTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
