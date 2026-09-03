/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 加密格式接口
*/
#pragma once

#include "audio_sdk/audio_types.h"   // AudioSdkState
#include <cstddef>
#include <cstdint>


class CEncryptedFormat{
public:
   
   static constexpr size_t   kAencPrefixSize = 6;           // 前缀长度：魔数 4 + version 2
   static constexpr uint16_t kAencVersion = 1;              // 格式版本（将来布局变化时好区分）

   // 判断内存中的文件头是不是 .aenc 加密容器
   static bool IsAencFile(const uint8_t* data, size_t size);

   static AudioSdk::AudioSdkState SaveAencFile(const wchar_t* filePath,const void* pcmData,
      size_t pcmSize);

   static void XorCrypt(uint8_t* data, size_t n);

private:
   // 密钥只存在 .cpp 里，绝不放进头文件
   static const uint8_t kKey[16];
};
