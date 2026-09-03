/* @Created On : 2026/8/10
   @Author : 孟源
   @note : wav文件格式处理接口
*/
#pragma once
#include <cstdint>

#include "audio_sdk/audio_types.h"

// WAV 文件头结构（PCM 格式，44 字节）
#pragma pack(push, 1)
struct WavHeader
{
    // RIFF 块
    char     riffId[4];      // "RIFF"
    uint32_t riffSize;       // 文件总长 - 8
    char     waveId[4];      // "WAVE"

    // fmt 子块
    char     fmtId[4];       // "fmt "
    uint32_t fmtSize;        // fmt 块长度（PCM 固定 16）
    uint16_t audioFormat;    // 1 = PCM
    uint16_t numChannels;    // 声道数
    uint32_t sampleRate;     // 采样率
    uint32_t byteRate;       // 每秒字节数
    uint16_t blockAlign;     // 每帧字节数
    uint16_t bitsPerSample;  // 位深

    // data 子块
    char     dataId[4];      // "data"
    uint32_t dataSize;       // PCM 数据长度
};
#pragma pack(pop)

static_assert(sizeof(WavHeader) == 44, "Wav头必须是44字节");

class CWavFormat
{
public:
    CWavFormat() = default;
    ~CWavFormat() = default;

    static AudioSdk::AudioSdkState SaveWavFile(const wchar_t* filePath, const void* data, size_t dataSize);

    static void FillHeader(WavHeader& hdr, uint32_t dataSize,
                uint32_t sampleRate    = SAMPLE_RATE,
                uint16_t channels      = CHANNELS,
                uint16_t bitsPerSample = BITS_PER_SAMPLE);
};