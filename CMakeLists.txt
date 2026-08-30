cmake_minimum_required(VERSION 3.16)
project(talktoclaude LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "talktoclaude targets Windows (WASAPI + Win32 SendInput/hooks).")
endif()

# whisper.cpp — vendored under third_party/, built as a static lib we link into our exe.
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/whisper.cpp)

add_executable(talktoclaude
    src/main.cpp
    src/audio_capture.cpp
    src/trigger.cpp
    src/text_injector.cpp
    src/transcriber.cpp
)

target_include_directories(talktoclaude PRIVATE
    third_party/whisper.cpp/include
    third_party/whisper.cpp/ggml/include
)

target_link_libraries(talktoclaude PRIVATE
    whisper
    ole32       # COM (WASAPI)
    user32      # SendInput, hooks
    winmm       # timing helpers
)

target_compile_definitions(talktoclaude PRIVATE UNICODE _UNICODE)
