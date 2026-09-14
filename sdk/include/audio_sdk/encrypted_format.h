/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 加密格式接口
*/
#pragma once

#include "audio_sdk/audio_export.h"
#include "audio_sdk/audio_types.h"   // AudioSdkState
#include <cstddef>
#include <cstdint>


class CEncryptedFormat{
public:
   
   static constexpr size_t   kAencPrefixSize = 6;           // 前缀长度：魔数 4 + version 2
   static constexpr uint16_t kAencVersion = 1;              // 格式版本（将来布局变化时好区分）
   static constexpr char     kAencMagic[4] = {'A','E','N','C'};  // 容器魔数（文件前 4 字节）
   static constexpr size_t   kAencMagicSize = 4;

   // 判断一段内存的开头是不是 .aenc 魔数（只比魔数, 不校验内部 WAV 头）。
   static bool IsAencData(const uint8_t* data, size_t size);
   // 判断某个文件是不是 .aenc 加密容器（自己读文件头, 不整个读进来）。
   static bool IsAencFile(const char* utf8Path);

   static AudioSdk::AudioSdkState SaveAencFile(const char* filePath,const void* pcmData,
      size_t pcmSize);

   static void XorCrypt(uint8_t* data, size_t n);

private:
   // 密钥只存在 .cpp 里，绝不放进头文件
   static const uint8_t kKey[16];
};
