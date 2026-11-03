cmake_minimum_required(VERSION 3.24)
project(RecDeskNative VERSION 0.2.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "RecDesk Native currently requires Windows 10/11")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(recdesk_core
  src/platform.cpp
  src/process.cpp
  src/audio.cpp
  src/audio_processing.cpp
  src/recorder.cpp
)
target_include_directories(recdesk_core PUBLIC src)
target_compile_definitions(recdesk_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(recdesk_core PUBLIC ole32 uuid avrt shell32 shlwapi user32 gdi32 comctl32 comdlg32)
if(MSVC)
  target_compile_options(recdesk_core PUBLIC /W4 /permissive- /utf-8 /EHsc)
endif()

add_executable(RecDesk WIN32 src/main.cpp src/region_selector.cpp)
target_link_libraries(RecDesk PRIVATE recdesk_core dwmapi uxtheme)
if(MSVC)
  target_link_options(RecDesk PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")
endif()

set(RECDESK_FFMPEG "" CACHE FILEPATH "Optional ffmpeg.exe copied beside RecDesk.exe")
if(RECDESK_FFMPEG)
  add_custom_command(TARGET RecDesk POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${RECDESK_FFMPEG}" "$<TARGET_FILE_DIR:RecDesk>/ffmpeg.exe")
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(recdesk_tests tests/core_tests.cpp)
  target_link_libraries(recdesk_tests PRIVATE recdesk_core)
  add_test(NAME recdesk_core COMMAND recdesk_tests)
  add_executable(recdesk_audio_tests tests/audio_tests.cpp)
  target_link_libraries(recdesk_audio_tests PRIVATE recdesk_core)
  add_test(NAME recdesk_audio COMMAND recdesk_audio_tests)
  set_tests_properties(recdesk_audio PROPERTIES TIMEOUT 60)
  add_executable(recdesk_export_tests tests/export_tests.cpp)
  target_link_libraries(recdesk_export_tests PRIVATE recdesk_core)
  add_test(NAME recdesk_export COMMAND recdesk_export_tests)
  set_tests_properties(recdesk_export PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
  add_executable(recdesk_ui_tests tests/ui_tests.cpp)
  target_link_libraries(recdesk_ui_tests PRIVATE recdesk_core)
  add_dependencies(recdesk_ui_tests RecDesk)
  add_test(NAME recdesk_ui COMMAND recdesk_ui_tests)
  set_tests_properties(recdesk_ui PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 40 RUN_SERIAL TRUE)
  add_executable(recdesk_smoke tests/smoke.cpp)
  target_link_libraries(recdesk_smoke PRIVATE recdesk_core)
endif()

install(TARGETS RecDesk RUNTIME DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" DESTINATION .)
if(RECDESK_FFMPEG)
  install(FILES "${RECDESK_FFMPEG}" DESTINATION .)
endif()
