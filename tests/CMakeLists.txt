cmake_minimum_required(VERSION 3.16)

# 测试与 sdk/ 平级: 单独一个工程, 不参与 SDK 交付物的构建。
# 编的时候把 SDK 的源码直接编译进来(不走 DLL), 所以测试不依赖 audio_sdk.dll 是否已部署。
project(audio_sdk_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /wd4819)
endif()

# 用例里用了 Sleep / GetTickCount, 目前只有 Windows 版
if(NOT WIN32)
    return()
endif()

set(SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../sdk)

# 源文件清单由 sdk/CMakeLists.txt 用 PARENT_SCOPE 交出来 —— 集中一处,
# 新加 .cpp 时不会出现"SDK 编了、测试漏了"的 LNK2019。
if(NOT DEFINED PLATFORM_SOURCES OR NOT DEFINED SDK_COMMON_SOURCES)
    message(FATAL_ERROR "tests: 请从仓库根目录构建(根 CMakeLists 会先 add_subdirectory(sdk))")
endif()

# 测试目录自己再 find_package 一次: 导入目标(Opus::opus 等)只在定义它的目录
# 及其子目录可见, 而 tests/ 是 sdk/ 的兄弟目录
list(APPEND CMAKE_PREFIX_PATH "${SDK_ROOT}/src/platform/windows/vorbis")
list(APPEND CMAKE_PREFIX_PATH "${SDK_ROOT}/src/platform/windows/opus")
find_package(Vorbis CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)

# 清单里是相对 sdk/ 的路径, 拿到这边要补成绝对路径, 否则会去 tests/src/ 下面找
set(SDK_SOURCES)
foreach(_src IN LISTS PLATFORM_SOURCES SDK_COMMON_SOURCES)
    list(APPEND SDK_SOURCES "${SDK_ROOT}/${_src}")
endforeach()

set(SDK_TEST_SOURCES
    src/test_positive.cpp
    src/test_negative.cpp
    src/test_boundary.cpp
    src/test_abnormal.cpp
)

foreach(test_src IN LISTS SDK_TEST_SOURCES)
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name}
        ${test_src}
        ${SDK_SOURCES}
        ${SDK_ROOT}/src/common/wav_validate.cpp
    )
    target_include_directories(${test_name} PRIVATE
        ${SDK_ROOT}/include
        ${SDK_ROOT}/logging/include
    )

    # opusenc 的 PUBLIC include 会把 <opus/opusenc.h> 和它引的 <opus/opus.h> 带进来
    target_link_libraries(${test_name} PRIVATE
        ${PLATFORM_LIBS}
        Vorbis::vorbisenc Vorbis::vorbisfile
        opusenc)
    target_compile_definitions(${test_name} PRIVATE AUDIO_SDK_STATIC)
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test
    )
endforeach()
