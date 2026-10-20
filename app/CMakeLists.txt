# app: the runnable llmbridge gateway daemon. The gateway logic
# lives in the `gateway` library module; this is just the CLI/signal shell.

# The option table (flags, config file, --help) as a library, so it is tested
# without running main().
add_library(llmbridge_options STATIC options.cpp routing_policy.cpp observability.cpp management_server.cpp)
target_link_libraries(llmbridge_options PUBLIC gateway provider)
target_include_directories(llmbridge_options PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(llmbridge main.cpp)
target_link_libraries(llmbridge PRIVATE llmbridge_options gateway provider)

# Install the daemon (so `cmake --install` puts `llmbridge` on path). Top-level only.
if (PROJECT_IS_TOP_LEVEL)
    install(TARGETS llmbridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

# ── CLI refusal tests ────────────────────────────────────────────────────────
# These guards live in main.cpp, which no unit test links, so until 2026-08-12
# nothing covered them. That is how --listen-tls came to be silently ignored in a
# non-TLS build: accepted, listener plaintext, operator none the wiser. Each case
# asserts the exact message, so a guard cannot pass by refusing for another reason.
if (LLMBRIDGE_BUILD_TESTS)
    function(llmbridge_cli_refuses name expect)
        add_test(NAME ${name} COMMAND llmbridge ${ARGN})
        # Timeout matters: a guard that fails to fire leaves a gateway running, so
        # without it the regression hangs the suite instead of failing it.
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}" TIMEOUT 10)
    endfunction()

    llmbridge_cli_refuses(CliRefusesListenTlsWithoutCert
            "a TLS listener needs a certificate and a key"
            --listen 0 --upstream 127.0.0.1:9 --listen-tls)
    llmbridge_cli_refuses(CliRefusesConfigWithoutPath
            "--config needs a path"
            --config)
    llmbridge_cli_refuses(CliRefusesDuplicateConfig
            "given twice"
            --config /nonexistent/a.json --config /nonexistent/b.json)
    llmbridge_cli_refuses(CliRefusesUnreadableConfig
            "cannot read"
            --config /nonexistent/llmbridge.json)
    # An unrecognised flag used to fall through the parse loop in silence, so the
    # gateway served the default port and the default upstream while the operator
    # believed it had been configured. `--listen=8080` is the shape that found it.
    llmbridge_cli_refuses(CliRefusesEqualsFormFlag
            "flags take a separate value"
            --listen=8080 --upstream 127.0.0.1:9)
    llmbridge_cli_refuses(CliRefusesUnknownFlag
            "unknown argument"
            --listen 0 --upstream 127.0.0.1:9 --lisen 8080)

    # --duration took atoi, so 0.5 became 0 and ran until killed; the timeout fails it.
    add_test(NAME CliDurationTakesAFraction
            COMMAND llmbridge --legacy-mode --listen 0 --upstream 127.0.0.1:9 --duration 0.5)
    set_tests_properties(CliDurationTakesAFraction PROPERTIES TIMEOUT 20)
    llmbridge_cli_refuses(CliRefusesImplicitLegacyMode
            "choose security.enabled=true"
            --listen 0 --upstream 127.0.0.1:9)
    llmbridge_cli_refuses(CliRefusesConflictingSecurityModes
            "choose security.enabled=true"
            --config ${CMAKE_CURRENT_SOURCE_DIR}/tests/conflicting_security.json)
    llmbridge_cli_refuses(CliRefusesSecurePlaintextListener
            "security mode requires a TLS listener"
            --config ${CMAKE_CURRENT_SOURCE_DIR}/tests/secure_plaintext.json)
    llmbridge_cli_refuses(CliRefusesSecurePlaintextUpstream
            "security mode refuses a plaintext non-loopback upstream"
            --config ${CMAKE_CURRENT_SOURCE_DIR}/tests/secure_plaintext_upstream.json)
    # The deadline used to be a sleep that SIGINT could not cut short.
    add_test(NAME CliSigintEndsADurationRun
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/sigint_ends_duration.sh
            $<TARGET_FILE:llmbridge>)
    set_tests_properties(CliSigintEndsADurationRun PROPERTIES TIMEOUT 40)
    llmbridge_cli_refuses(CliRefusesBadPort "out of range"
            --listen 70000 --upstream 127.0.0.1:9)

    llmbridge_cli_refuses(CliRefusesCertWithoutListenTls
            "certificate/key was given without enabling the"
            --listen 0 --upstream 127.0.0.1:9 --tls-cert /nonexistent --tls-key /nonexistent)

    if (NOT LLMBRIDGE_TLS)
        # Both fail-open cases a non-TLS build must refuse instead of downgrading.
        llmbridge_cli_refuses(CliRefusesListenTlsWithoutTlsBuild
                "requires a TLS build"
                --listen 0 --upstream 127.0.0.1:9 --listen-tls
                --tls-cert /nonexistent --tls-key /nonexistent)
        llmbridge_cli_refuses(CliRefusesHttpsUpstreamWithoutTlsBuild
                "https:// upstream requires a TLS build"
                --legacy-mode --listen 0 --upstream https://example.invalid)
    endif ()
endif ()

if (LLMBRIDGE_BUILD_TESTS)
    include(GoogleTest)
    add_executable(app_options_test tests/config_test.cpp tests/cli_test.cpp tests/routing_policy_test.cpp)
    target_link_libraries(app_options_test PRIVATE llmbridge_options GTest::gtest_main)
    target_compile_definitions(app_options_test PRIVATE
            LLMBRIDGE_EXAMPLE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/llmbridge.example.json")
    gtest_discover_tests(app_options_test)

    # 手动运行的选路微基准，不作为有时序阈值的 CTest。
    add_executable(routing_policy_bench tests/routing_policy_bench.cpp)
    target_link_libraries(routing_policy_bench PRIVATE llmbridge_options)

    # AI-llm-transplot 端到端测试：真实启动网关进程，验证主上游不可达时切换到备用上游。
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_Interpreter_FOUND)
        add_test(NAME AppMultiUpstreamFailover
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/multi_upstream_e2e.py
                $<TARGET_FILE:llmbridge>)
        set_tests_properties(AppMultiUpstreamFailover PROPERTIES TIMEOUT 20)
        add_test(NAME AppMultiUpstreamBoundaries
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/multi_upstream_boundaries.py
                $<TARGET_FILE:llmbridge>)
        set_tests_properties(AppMultiUpstreamBoundaries PROPERTIES TIMEOUT 40)
        add_test(NAME AppStatusRetry
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/status_retry_e2e.py
                $<TARGET_FILE:llmbridge>)
        set_tests_properties(AppStatusRetry PROPERTIES TIMEOUT 30)
        add_test(NAME AppHotReload
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/hot_reload_e2e.py
                $<TARGET_FILE:llmbridge>)
        set_tests_properties(AppHotReload PROPERTIES TIMEOUT 40)
        add_test(NAME AppModelPools
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/model_pools_e2e.py
                $<TARGET_FILE:llmbridge>)
        set_tests_properties(AppModelPools PROPERTIES TIMEOUT 30)
        add_test(NAME AppManagement
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/management_e2e.py
                $<TARGET_FILE:llmbridge>)
        set_tests_properties(AppManagement PROPERTIES TIMEOUT 30)
        add_test(NAME AppSecureUsers
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/secure_users_e2e.py
                $<TARGET_FILE:llmbridge>)
        set_tests_properties(AppSecureUsers PROPERTIES TIMEOUT 30)
    endif ()
endif ()
