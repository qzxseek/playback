/* @Created On : 2026/8/10
   @Author : 孟源
   @note : wav文件格式处理实现
*/
#include "audio_sdk/wav_format.h"   // 内含 audio_types.h
#include "audio_sdk/encrypted_format.h"   // bEncrypt=true 时委托存成 .aenc 容器
#include "audio_sdk/logging.h"

#include <cerrno>   // errno / ENOSPC: 写失败时判是不是盘满
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>      // std::bad_alloc

static constexpr const char* kTag = "wav";   // 日志来源标记

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
 * @brief 打开文件, 先把占位头落盘
 * @param utf8Path 输出文件路径(UTF-8)
 * @param bEncrypt 是否存加密容器(.aenc)
 * @return 操作状态
 */
AudioSdk::AudioSdkState CWavStreamWriter::Open(const char* utf8Path, bool bEncrypt){
    Finish();                       // 上一轮没关干净就收尾

    if (!utf8Path)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    try {
        m_file.clear();             
        m_file.open(std::filesystem::u8path(utf8Path),
                    std::ios::out | std::ios::binary | std::ios::trunc);
        if (!m_file.is_open())
            return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

        m_encrypt = bEncrypt;
        m_written = 0;
        m_headerPos = 0;
        m_path = utf8Path;          // 写失败时要用它查剩余空间 + 记日志

        if (bEncrypt) {             // .aenc 壳: 魔数 + 版本, 排在 WAV 头前面
            m_file.write(CEncryptedFormat::kAencMagic, CEncryptedFormat::kAencMagicSize);
            const uint16_t version = CEncryptedFormat::kAencVersion;
            m_file.write(reinterpret_cast<const char*>(&version), sizeof(version));
            m_headerPos = static_cast<std::streamoff>(CEncryptedFormat::kAencPrefixSize);
        }

        WavHeader hdr;
        CWavFormat::FillHeader(hdr, 0);   // 长度先记 0, 收尾再回填
        m_file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        // 头立刻刷出去
        m_file.flush();
        if (!m_file.good()) {
            m_file.close();
            return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
        }
    } catch (const std::bad_alloc&) {
        m_file.close();
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
    } catch (const std::filesystem::filesystem_error&) {
        m_file.close();
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
    } catch (std::exception&) {
        m_file.close();
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
    }
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 追加一块 PCM(加密时就地 XOR, 不额外拷贝)
 * @param pcm PCM 数据, 会被就地加密改写
 * @param n 字节数
 * @return NONE = 已写进文件; FILE_OPEN_FAILED = 没打开; FILE_WRITE_FAILED = 写不进去
 */
AudioSdk::AudioSdkState CWavStreamWriter::Write(void* pcm, size_t n){
    if (!m_file.is_open())
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
    if (n == 0)
        return AudioSdk::AudioSdkState::NONE;                // 空块不算失败(录音刚起可能给出 0 字节的块)

    uint8_t* p = static_cast<uint8_t*>(pcm);
    if (m_encrypt)
        CEncryptedFormat::XorCrypt(p, n, static_cast<size_t>(m_written));   // 密文与整段加密等长

    try {
        m_file.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
    } catch (const std::bad_alloc&) {
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;      // 缓冲扩容失败(Open 后本不该再分配, 兜底)
    }

    if (!m_file.good()) {
        
        const int err = errno;
        std::error_code ec;
        const std::filesystem::space_info sp = std::filesystem::space(m_path, ec);
        const unsigned long long availMB =
            ec ? 0ULL : static_cast<unsigned long long>(sp.available / 1048576ULL);

        AUDIO_LOG_ERROR(kTag, "写失败 \"%s\": errno=%d%s%s, 已写 %llu 字节, 本次 %zu 字节, 剩余 %lluMB",
                        m_path.c_str(), err,
                        err == ENOSPC ? " [ENOSPC 磁盘空间不足]" : "",
                        ec ? " [拿不到剩余空间]" : "",
                        static_cast<unsigned long long>(m_written), n, availMB);
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
    }

    m_written += n;
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 回填真实长度并关闭文件
 * @return true = 头与尾部都落了盘; false = 文件是残的(头还是 0 长度或尾巴没刷出去)
 */
bool CWavStreamWriter::Finish(){
    if (!m_file.is_open())
        return true;                

    WavHeader hdr;
    CWavFormat::FillHeader(hdr, static_cast<uint32_t>(m_written));  
    try {
        m_file.seekp(m_headerPos);
        if (m_file.good())
            m_file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    } catch (std::exception&) {
        // 析构也会走到这儿(析构里抛 = 直接终止进程), 吞掉: 返回值已经说明文件是残的
    }

    m_file.close();                 
    return m_file.good();           
}

CWavStreamWriter::~CWavStreamWriter(){
    Finish();                       
}

/**
 * @brief 将一段 PCM 数据保存为文件(明文 WAV 或加密 .aenc, 由 bEncrypt 决定)
 * @param filePath 文件路径
 * @param data PCM 数据指针
 * @param dataSize PCM 数据长度
 * @param bEncrypt 是否存为加密容器(true = .aenc; false = 明文 WAV)
 * @return AudioSdkState 操作状态
*/
AudioSdk::AudioSdkState CWavFormat::SaveWavFile(const char* filePath, const void* data,
    size_t dataSize,bool bEncrypt){
    // 路径为NULL
    if (!filePath)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    // 加密
    if (bEncrypt)
        return CEncryptedFormat::SaveAencFile(filePath, data, dataSize);

    try {
        std::ofstream file(std::filesystem::u8path(filePath), std::ios::out | std::ios::binary);
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
    } catch (const std::bad_alloc&) {
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
    } catch (const std::filesystem::filesystem_error&) {
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
    } catch (std::exception&) {
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
    }
    return AudioSdk::AudioSdkState::NONE;
}
