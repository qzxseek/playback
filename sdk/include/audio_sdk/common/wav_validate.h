/* @Created On : 2026/8/11
   @Author : 孟源
   @note : WAV 文件校验类
*/
#pragma once

#include "audio_sdk/audio_types.h"
#include "audio_sdk/wav_format.h"
#include <cstddef>
#include <cstdint>

class WavValidate
{
public:
    WavValidate();
    ~WavValidate();

    // 校验内存中的整个 WAV 文件（文件头 + PCM 数据）
    // data/size 来自读取整文件的缓冲；校验通过返回 true。
    bool Validate(const uint8_t* data, size_t size);

    // 校验通过后，返回解析出的文件头（用于构建 WAVEFORMATEX 等）
    const WavHeader& Header() const { return m_header; }

private:
    bool ReadHeader(const uint8_t* data, size_t size);
    bool ValidateHeader(size_t size);
    WavHeader m_header{};
};
