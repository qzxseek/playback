/* @Created On : 2026/8/11
   @Author : 孟源
   @note : WAV 文件校验类
*/
#include "audio_sdk/common/wav_validate.h"

#include <cstring>
#include <cstdint>

WavValidate::WavValidate() = default;
WavValidate::~WavValidate() = default;

/*
 * @brief 校验内存中的 WAV 文件
 * @param data 整个 WAV 文件的内存缓冲（44 字节头 + PCM 数据）
 * @param size 缓冲长度
 * @return 校验通过返回 true
 */
bool WavValidate::Validate(const uint8_t* data, size_t size){
    return ReadHeader(data, size) && ValidateHeader(size);
}

/*
 * @brief 从内存缓冲拷贝 WAV 文件头
 * @param data 文件缓冲
 * @param size 缓冲长度
 * @return 拷贝成功返回 true
 */
bool WavValidate::ReadHeader(const uint8_t* data, size_t size){
    // 指针为空，或长度不足 44 字节，说明文件被截断或文件头损坏。
    if (!data || size < sizeof(m_header))
        return false;

    std::memcpy(&m_header, data, sizeof(m_header));
    return true;
}

/*
 * @brief 校验 WAV 文件头内容
 * @param size 文件实际总长度
 * @return 校验通过返回 true
 */
bool WavValidate::ValidateHeader(size_t size){
    // 校验 RIFF、WAVE、fmt、data 四个关键标识。
    if (std::memcmp(m_header.riffId, "RIFF", 4) != 0 ||
        std::memcmp(m_header.waveId, "WAVE", 4) != 0 ||
        std::memcmp(m_header.fmtId, "fmt ", 4) != 0 ||
        std::memcmp(m_header.dataId, "data", 4) != 0) {
        return false;
    }

    // 当前只支持标准 PCM WAV：音频格式编号为 1，fmt 块大小必须为 16 字节。
    // 声道数、采样率和位深不能为 0，位深必须是完整字节的倍数。
    if (m_header.fmtSize != 16 ||
        m_header.audioFormat != 1 ||
        m_header.numChannels == 0 ||
        m_header.sampleRate == 0 ||
        m_header.bitsPerSample == 0 ||
        (m_header.bitsPerSample % 8) != 0) {
        return false;
    }

    const uint32_t expectedBlockAlign =
        static_cast<uint32_t>(m_header.numChannels) *
        (m_header.bitsPerSample / 8);
    const uint32_t expectedByteRate =
        m_header.sampleRate * expectedBlockAlign;

    if (m_header.blockAlign != expectedBlockAlign ||
        m_header.byteRate != expectedByteRate ||
        m_header.dataSize % m_header.blockAlign != 0) {
        return false;
    }

    // 文件实际大小与头信息一致（当前 WavHeader 不支持额外的 WAV 子块）。
    const uint64_t fileSize = static_cast<uint64_t>(size);
    const uint64_t riffFileSize =
        static_cast<uint64_t>(m_header.riffSize) + 8u;
    const uint64_t dataEnd =
        sizeof(m_header) + static_cast<uint64_t>(m_header.dataSize);

    return riffFileSize == fileSize && dataEnd == fileSize;
}
