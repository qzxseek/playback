/* @Created On : 2026/8/10
   @Author : 孟源
   @note : wav文件格式处理接口
*/
#pragma once
#include <cstdint>
#include <fstream>
#include <string>

#include "audio_sdk/audio_types.h"
#include "audio_sdk/audio_export.h"

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
    char     dataId[4];      // "data"(仅标准排布)
    uint32_t dataSize;       // PCM 数据长度(仅标准排布)
};
#pragma pack(pop)

static_assert(sizeof(WavHeader) == 44, "Wav头必须是44字节");

class CWavFormat
{
public:
    CWavFormat() = default;
    ~CWavFormat() = default;

  
    static AudioSdk::AudioSdkState SaveWavFile(const char* filePath, const void* data,
        size_t dataSize,bool bEncrypt = false);

    static void FillHeader(WavHeader& hdr, uint32_t dataSize,
                uint32_t sampleRate    = SAMPLE_RATE,
                uint16_t channels      = CHANNELS,
                uint16_t bitsPerSample = BITS_PER_SAMPLE);
};

/**
 * @brief 边录边写器: 开文件先落占位头 → 逐块追加密文/明文 → 收尾回填真实长度
 */
class CWavStreamWriter{
public:
    ~CWavStreamWriter();                 // 兜底: 忘了 Finish 也要把头补上再关

    // 打开文件并写好占位头(dataSize 记 0)。bEncrypt: true 存 .aenc, false 存明文 WAV
    AudioSdk::AudioSdkState Open(const char* utf8Path, bool bEncrypt);

    // 追加一块 PCM
    AudioSdk::AudioSdkState Write(void* pcm, size_t n);
    bool Finish();

    uint64_t GetWritten() const { return m_written; }

private:
    std::ofstream  m_file;
    bool           m_encrypt = false;
    uint64_t       m_written = 0;       // 已写 PCM 字节数(也是下一块的 XOR 偏移)
    std::streamoff m_headerPos = 0;     // 44 字节 WAV 头在文件里的偏移
    std::string    m_path;              // 落盘路径: 写失败时用它查剩余空间 + 记日志
};