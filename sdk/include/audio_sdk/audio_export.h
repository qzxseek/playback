#pragma once

#if defined(AUDIO_SDK_STATIC)
  #define AUDIO_API
#elif defined(_WIN32)
  #if defined(AUDIO_SDK_BUILD)     // 编译 SDK 本体时定义这个宏
    #define AUDIO_API __declspec(dllexport)   // 我在生产导出
  #else
    #define AUDIO_API __declspec(dllimport)   // 使用方在消费导入
  #endif
#else   // Android/Linux/macOS
  #define AUDIO_API __attribute__((visibility("default")))
#endif
