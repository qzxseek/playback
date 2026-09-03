/* @Created On : 2026/8/10
   @Author : 孟源
   @note : wav文件格式处理实现
*/
#include "audio_sdk/wav_format.h"   // 内含 audio_types.h
#include "audio_sdk/encrypted_format.h"   // bEncrypt=true 时委托存成 .aenc 容器

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

/**
 * @brief 填充 WAV 文件头
 * @param hdr WAV 文件头结构体引用
 * @param dataSize PCM 数据长度
 * @param sampleRate 采样率
 * @param channels 声道数
 * @param bitsPerSample 位深
*/
void CWavFormat::FillHeader(WavHeader& hdr, uint32_t dataSize,uint32_t sampleRate,
                uint16_t channels,uint16_t bitsPerSample){
    uint16_t blockAlign = channels * (bitsPerSample / 8);
    uint32_t byteRate   = sampleRate * blockAlign;

    std::memcpy(hdr.riffId, "RIFF", 4);
    hdr.riffSize       = 36 + dataSize;         // 文件总长 - 8 = (44-8) + dataSize
    std::memcpy(hdr.waveId, "WAVE", 4);

    std::memcpy(hdr.fmtId, "fmt ", 4);
    hdr.fmtSize        = 16;                     // PCM
    hdr.audioFormat    = 1;                      // PCM = 1
    hdr.numChannels    = channels;
    hdr.sampleRate     = sampleRate;
    hdr.byteRate       = byteRate;
    hdr.blockAlign     = blockAlign;
    hdr.bitsPerSample  = bitsPerSample;

    std::memcpy(hdr.dataId, "data", 4);
    hdr.dataSize       = dataSize;
}

/**
 * @brief 将一段 PCM 数据保存为文件(明文 WAV 或加密 .aenc, 由 bEncrypt 决定)
 * @param filePath 文件路径
 * @param data PCM 数据指针
 * @param dataSize PCM 数据长度
 * @param bEncrypt 是否存为加密容器(true = .aenc; false = 明文 WAV)
 * @return AudioSdkState 操作状态
*/
AudioSdk::AudioSdkState CWavFormat::SaveWavFile(const wchar_t* filePath, const void* data, 
    size_t dataSize,bool bEncrypt){
    // 加密
    if (bEncrypt)
        return CEncryptedFormat::SaveAencFile(filePath, data, dataSize);

    // 明文
    std::ofstream file(filePath, std::ios::out | std::ios::binary);
    if (!file.is_open())
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;   // 打开文件失败

    // 写 WAV 文件头
    WavHeader hdr;
    CWavFormat::FillHeader(hdr, static_cast<uint32_t>(dataSize));
    file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!file.good())
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;   // 写入文件失败

    // 写明文 PCM 数据
    if (dataSize > 0)
        file.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(dataSize));
    if (!file.good())
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;   // 写入文件失败

    file.close();
    return AudioSdk::AudioSdkState::NONE;
}
