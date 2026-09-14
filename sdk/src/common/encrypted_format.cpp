/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 加密格式实现
*/
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/wav_format.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

// 加密密钥
const uint8_t CEncryptedFormat::kKey[16] = {
    0x51,0x23,0x8A,0x5B,0xC4,0x69,0x02,0x7D,
    0xE1,0x9C,0x37,0xF0,0x4B,0xAD,0x66,0x88
};

/**
 * @brief XOR 对称加解密（自逆：加密和解密是同一个函数）
 * @param data 待处理数据（原地）
 * @param n 数据长度
 */
void CEncryptedFormat::XorCrypt(uint8_t* data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        data[i] ^= kKey[i % 16];
}

/**
 * @brief 判断一段内存的开头是不是 .aenc 魔数
 * @param data 内存起始地址（通常是文件开头）
 * @param size 数据长度
 * @return 是 .aenc 返回 true（只比魔数, 不校验内部 WAV 头）
 */
bool CEncryptedFormat::IsAencData(const uint8_t* data, size_t size)
{
    return data && size >= kAencMagicSize &&
           std::memcmp(data, kAencMagic, kAencMagicSize) == 0;
}

/**
 * @brief 判断某个文件是不是 .aenc 加密容器（只读文件头 4 字节）
 * @param utf8Path 文件路径（UTF-8）
 * @return 是加密容器返回 true; 打不开或文件太短返回 false
 */
bool CEncryptedFormat::IsAencFile(const char* utf8Path)
{
    if (!utf8Path)
        return false;

    std::ifstream file(std::filesystem::u8path(utf8Path), std::ios::in | std::ios::binary);
    if (!file.is_open())
        return false;                                 // 打不开就当不是（调用方只用来提示）

    uint8_t head[kAencMagicSize] = {};
    if (!file.read(reinterpret_cast<char*>(head), sizeof(head)))
        return false;                                 // 文件太短, 连魔数都放不下

    return IsAencData(head, sizeof(head));
}

/**
 * @brief 把一段 PCM 保存成加密容器文件  布局: [AENC 4B][version 2B][标准 WAV 头 44B][XOR 密文]
 * @param filePath 输出文件路径
 * @param pcmData 明文 PCM 指针
 * @param pcmSize 明文 PCM 长度
 * @return 操作状态
 */
AudioSdk::AudioSdkState CEncryptedFormat::SaveAencFile(const char* filePath,const void* pcmData,
                                                      size_t pcmSize)
{
    if (!filePath || (!pcmData && pcmSize > 0))
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;

    // 数据段加密（XOR 等长，直接做在明文拷贝上）
    std::vector<uint8_t> vecCipher(static_cast<const uint8_t*>(pcmData),
                              static_cast<const uint8_t*>(pcmData) + pcmSize);
    XorCrypt(vecCipher.data(), vecCipher.size());

    // UTF-8 路径 → 平台 native 编码(u8path): Windows 中文不乱码, Android/Linux 直接用
    std::ofstream file(std::filesystem::u8path(filePath), std::ios::out | std::ios::binary);
    if (!file.is_open())
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    // 魔数 + 版本（文件级封装标记）
    file.write(kAencMagic, kAencMagicSize);
    const uint16_t version = kAencVersion;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    if (!file.good())
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;

    // 标准 WAV 头
    WavHeader hdr;
    CWavFormat::FillHeader(hdr, static_cast<uint32_t>(pcmSize));
    file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!file.good())
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;

    // 写入密文
    file.write(reinterpret_cast<const char*>(vecCipher.data()), vecCipher.size());
    if (!file.good())
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;

    file.close();
    return AudioSdk::AudioSdkState::NONE;
}
