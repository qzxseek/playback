cmake_minimum_required(VERSION 3.16)

project(audio_sdk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


if(MSVC)
    add_compile_options(/utf-8)
endif()


add_library(audio_sdk SHARED
    src/audio_player.cpp
    src/audio_recorder.cpp
    src/wav_format.cpp
    src/encrypted_format.cpp
    src/common/wav_validate.cpp
)
target_include_directories(audio_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(audio_sdk PUBLIC winmm)

set_target_properties(audio_sdk PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

# 录制测试：正常多 TU 链接（CAudioRecorder 类，回调走 CALLBACK_FUNCTION，无需窗口）。
add_executable(test_record
    test/test_record.cpp
    src/audio_recorder.cpp
    src/wav_format.cpp
    src/encrypted_format.cpp
)
target_include_directories(test_record PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test_record PRIVATE winmm)

# 播放测试：正常多 TU 链接（CAudioPlayer 类；wav_format 提供测试音生成用的 SaveWavFile）。
add_executable(test_play
    test/test_play.cpp
    src/audio_player.cpp
    src/common/wav_validate.cpp
    src/wav_format.cpp
    src/encrypted_format.cpp
)
target_include_directories(test_play PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test_play PRIVATE winmm)
