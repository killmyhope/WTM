cmake_minimum_required(VERSION 3.24)
project(WindowManager VERSION 0.1.0 LANGUAGES CXX RC)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "WindowManager requires Windows.")
endif()

add_library(twm_layout
    src/geometry.cpp
    src/managed_windows.cpp
    src/master_stack.cpp
)
target_include_directories(twm_layout PUBLIC include)
target_compile_features(twm_layout PUBLIC cxx_std_20)

add_library(twm_core
    src/control_window.cpp
    src/monitor_context.cpp
    src/window_geometry.cpp
    src/window_actuator.cpp
    src/layout_controller.cpp
    src/window_classifier.cpp
    src/window_registry.cpp
    src/win_event_source.cpp
)
target_include_directories(twm_core PUBLIC include)
target_compile_features(twm_core PUBLIC cxx_std_20)
set_target_properties(twm_core PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(twm_core PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0A00 _WIN32_WINNT=0x0A00
)
target_link_libraries(twm_core PUBLIC twm_layout user32 dwmapi)

function(twm_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
twm_warnings(twm_core)
twm_warnings(twm_layout)

# Console subsystem: diagnostics are written to stdout, not a GUI window.
add_executable(window_diagnostics
    src/main.cpp
    src/console_logger.cpp
    resources/app.rc
)
target_link_libraries(window_diagnostics PRIVATE twm_core)
twm_warnings(window_diagnostics)
if(MSVC)
    # The resource already embeds the complete manifest.
    target_link_options(window_diagnostics PRIVATE /MANIFEST:NO)
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(stage4_test IN ITEMS layout_controller_tests window_actuator_tests)
        add_executable(${stage4_test} tests/${stage4_test}.cpp resources/app.rc)
        target_link_libraries(${stage4_test} PRIVATE twm_core)
        twm_warnings(${stage4_test})
        if(MSVC)
            target_link_options(${stage4_test} PRIVATE /MANIFEST:NO)
        endif()
        add_test(NAME ${stage4_test} COMMAND ${stage4_test})
        set_tests_properties(${stage4_test} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(geometry_tests tests/geometry_tests.cpp)
    target_link_libraries(geometry_tests PRIVATE twm_layout)
    twm_warnings(geometry_tests)
    add_test(NAME geometry_conversions COMMAND geometry_tests)

    foreach(stage3_test IN ITEMS window_geometry_tests monitor_context_tests)
        add_executable(${stage3_test} tests/${stage3_test}.cpp resources/app.rc)
        target_link_libraries(${stage3_test} PRIVATE twm_core)
        twm_warnings(${stage3_test})
        if(MSVC)
            target_link_options(${stage3_test} PRIVATE /MANIFEST:NO)
        endif()
        add_test(NAME ${stage3_test} COMMAND ${stage3_test})
        set_tests_properties(${stage3_test} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(layout_tests tests/layout_tests.cpp)
    target_link_libraries(layout_tests PRIVATE twm_layout)
    twm_warnings(layout_tests)
    add_test(NAME master_stack_solver COMMAND layout_tests)

    add_executable(managed_windows_tests tests/managed_windows_tests.cpp)
    target_link_libraries(managed_windows_tests PRIVATE twm_layout)
    twm_warnings(managed_windows_tests)
    add_test(NAME managed_window_order COMMAND managed_windows_tests)

    add_executable(registry_tests tests/registry_tests.cpp)
    target_link_libraries(registry_tests PRIVATE twm_core)
    twm_warnings(registry_tests)
    add_test(NAME registry_transitions COMMAND registry_tests)

    add_executable(native_tests tests/native_tests.cpp)
    target_link_libraries(native_tests PRIVATE twm_core)
    twm_warnings(native_tests)
    add_test(NAME native_windows_and_events COMMAND native_tests)
    set_tests_properties(native_windows_and_events PROPERTIES TIMEOUT 20)
endif()
