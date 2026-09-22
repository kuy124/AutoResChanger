cmake_minimum_required(VERSION 3.16)
project(AutoResChanger LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "AutoRes Changer targets Windows only.")
endif()

# Prefer MinGW-w64 (MSYS2) or MSVC. The code is portable across both, but the
# #pragma comment(lib, ...) directives are MSVC-only and simply ignored by GCC,
# so the libraries are listed explicitly below for the MinGW path.
add_executable(AutoResChanger WIN32 main.cpp resource.rc)

target_compile_definitions(AutoResChanger PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(AutoResChanger PRIVATE /W4 /permissive-)
    target_link_libraries(AutoResChanger PRIVATE
        user32 gdi32 shell32 comdlg32 advapi32 dwmapi uxtheme)
    set_target_properties(AutoResChanger PROPERTIES
        LINK_FLAGS "/ENTRY:wWinMainCRTStartup")
else()
    # MinGW-w64
    target_compile_options(AutoResChanger PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unknown-pragmas
        -Wno-missing-field-initializers)
    # -municode selects wWinMain as the entry point.
    target_link_options(AutoResChanger PRIVATE -municode)
    target_link_libraries(AutoResChanger PRIVATE
        user32 gdi32 shell32 comdlg32 advapi32 dwmapi uxtheme comctl32 ole32)
endif()

# Emit the binary next to the source tree for convenience.
set_target_properties(AutoResChanger PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- Tests --------------------------------------------------------------
# Headless unit tests (compiled with -DTESTING, which excludes the GUI).
enable_testing()
add_executable(autores_tests tests/test_main.cpp)
target_compile_definitions(autores_tests PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(autores_tests PRIVATE /W4 /permissive-)
    target_link_libraries(autores_tests PRIVATE user32 gdi32 shell32 comdlg32 advapi32 dwmapi ole32 comctl32)
else()
    target_compile_options(autores_tests PRIVATE -Wall -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers)
    target_link_libraries(autores_tests PRIVATE user32 gdi32 shell32 comdlg32 advapi32 dwmapi uxtheme ole32 comctl32)
endif()
add_test(NAME unit COMMAND autores_tests)
