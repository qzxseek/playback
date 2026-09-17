cmake_minimum_required(VERSION 3.16)

project(audio_win_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# UI 可执行程序: main.cpp(入口) + main_window.cpp(窗口类实现) + crash_dump.cpp(崩溃转储)
add_executable(audio_win_app
    main.cpp
    main_window.cpp
    crash_dump.cpp
)

set_target_properties(audio_win_app PROPERTIES WIN32_EXECUTABLE ON)

target_include_directories(audio_win_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 显式加载不需要导入库(.lib): 运行期靠 LoadLibrary("audio_sdk.dll") + GetProcAddress 自己找符号。
target_link_libraries(audio_win_app PRIVATE
    comdlg32                    
)

# dll: 运行时 LoadLibrary 会先在 exe 所在目录找, 构建后自动拷到 exe 旁边, 保证双击就能跑
add_custom_command(TARGET audio_win_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/resource/audio_sdk.dll
        $<TARGET_FILE_DIR:audio_win_app>
)
