cmake_minimum_required(VERSION 3.16)

project(audio_win_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# UI 可执行程序: main.cpp(入口) + main_window.cpp(窗口类实现)
add_executable(audio_win_app
    main.cpp
    main_window.cpp
)

set_target_properties(audio_win_app PROPERTIES WIN32_EXECUTABLE ON)

target_link_libraries(audio_win_app PRIVATE audio_sdk comdlg32)
