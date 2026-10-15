cmake_minimum_required(VERSION 3.24)
project(WindowManager VERSION 0.1.0 LANGUAGES CXX RC)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "WindowManager requires Windows.")
endif()

add_library(twm_core
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
target_link_libraries(twm_core PUBLIC user32 dwmapi)

function(twm_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
twm_warnings(twm_core)

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
