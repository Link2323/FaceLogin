add_executable(CredentialProviderInteractionPolicyTest
    credential_provider_interaction_policy_test.cpp
)

target_include_directories(CredentialProviderInteractionPolicyTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../credential_provider
)

target_compile_options(CredentialProviderInteractionPolicyTest PRIVATE /utf-8)
