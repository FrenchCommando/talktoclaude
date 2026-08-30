cmake_minimum_required(VERSION 3.16)
project(talktoclaude LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "talktoclaude targets Windows (WASAPI + Win32 SendInput/hooks).")
endif()

# whisper.cpp — fetched at configure time (pinned to a specific commit, not
# a moving branch) rather than vendored/submoduled. No separate "init
# submodules" step to remember; `cmake -B build` alone is enough.
include(FetchContent)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    whisper_cpp
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        c4ac0012a8f5a2082dfca6aad4ddfd8b2c02b337
)
# whisper.cpp's top-level CMakeLists still declares cmake_minimum_required
# below 3.10, which newer CMake warns about. It's upstream's file — we can't
# fix it here and re-fetching would undo an edit anyway — so just mute
# deprecation warnings while its subdirectory is added.
if(NOT DEFINED CMAKE_WARN_DEPRECATED)
  set(CMAKE_WARN_DEPRECATED ON)
endif()
set(_ttc_warn_deprecated ${CMAKE_WARN_DEPRECATED})
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper_cpp)
set(CMAKE_WARN_DEPRECATED ${_ttc_warn_deprecated} CACHE BOOL "" FORCE)

add_executable(talktoclaude
    src/main.cpp
    src/logging.cpp
    src/audio_capture.cpp
    src/trigger.cpp
    src/text_injector.cpp
    src/transcriber.cpp
)

target_include_directories(talktoclaude PRIVATE
    ${whisper_cpp_SOURCE_DIR}/include
    ${whisper_cpp_SOURCE_DIR}/ggml/include
)

target_link_libraries(talktoclaude PRIVATE
    whisper
    ole32       # COM (WASAPI)
    user32      # SendInput, hooks
    winmm       # timing helpers
    windowsapp  # WinRT projection (SystemMediaTransportControls)
)

target_compile_definitions(talktoclaude PRIVATE UNICODE _UNICODE)

# whisper/ggml build as shared libs into build/bin; Windows only auto-loads
# DLLs next to the exe (build/) or on PATH, so copy them alongside it.
add_custom_command(TARGET talktoclaude POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:whisper>
        $<TARGET_FILE:ggml>
        $<TARGET_FILE:ggml-base>
        $<TARGET_FILE:ggml-cpu>
        $<TARGET_FILE_DIR:talktoclaude>
)
