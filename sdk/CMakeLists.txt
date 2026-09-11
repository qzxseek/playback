cmake_minimum_required(VERSION 3.16)

project(audio_sdk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 平台相关 
if(WIN32)
    set(PLATFORM_SOURCES
        src/windows/audio_player.cpp
        src/windows/audio_recorder.cpp)
    set(PLATFORM_LIBS winmm)
elseif(ANDROID)
    set(PLATFORM_SOURCES
        src/android/audio_player.cpp
        src/android/audio_recorder.cpp)
    find_library(aaudio-lib aaudio)          
    set(PLATFORM_LIBS ${aaudio-lib})
else()
    message(FATAL_ERROR "audio_sdk: 暂不支持的平台 ${CMAKE_SYSTEM_NAME}")
endif()

# SDK 动态库 
add_library(audio_sdk SHARED
    ${PLATFORM_SOURCES}
    src/common/wav_format.cpp
    src/common/encrypted_format.cpp
    src/common/wav_validate.cpp
)

target_include_directories(audio_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(audio_sdk PUBLIC ${PLATFORM_LIBS})

# 告诉 audio_export.h: 现在编的是 SDK 本体 → AUDIO_API 展开成 dllexport
target_compile_definitions(audio_sdk PRIVATE AUDIO_SDK_BUILD)

# Windows 专用自测程序(依赖 winmm 设备层, Android 下不构建) 
if(WIN32)
    # 录制测试：正常多 TU 链接（CAudioRecorder 类，回调走 CALLBACK_FUNCTION，无需窗口）。
    add_executable(test_win_record
        test/win_test_record.cpp
        src/windows/audio_recorder.cpp
        src/common/wav_format.cpp
        src/common/encrypted_format.cpp
    )
    target_include_directories(test_win_record PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    # 测试程序把 SDK 源码静态编进来, 既不是导入也不是导出 → AUDIO_API 置空
    target_compile_definitions(test_win_record PRIVATE AUDIO_SDK_STATIC)
    target_link_libraries(test_win_record PRIVATE winmm)

    # 播放测试：正常多 TU 链接（CAudioPlayer 类；wav_format 提供测试音生成用的 SaveWavFile）。
    add_executable(test_win_play
        test/win_test_play.cpp
        src/windows/audio_player.cpp
        src/common/wav_validate.cpp
        src/common/wav_format.cpp
        src/common/encrypted_format.cpp
    )
    target_include_directories(test_win_play PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    # 同上: 静态编译 SDK 源码, 不要 dllimport/dllexport
    target_compile_definitions(test_win_play PRIVATE AUDIO_SDK_STATIC)
    target_link_libraries(test_win_play PRIVATE winmm)
endif()
