/* @Created On : 2026/10/16
   @Author : 孟源
   @note : opus 编码实现(压缩格式层)
*/
#include "audio_sdk/audio_types.h"
#include "audio_sdk/opus_format.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/logging.h"

#include <opus/opusenc.h>            // 源码在 tool/libopusenc

#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <new>

static constexpr const char* kTag = "opus";     // 日志来源标记

// 录音侧一个采样点(单声道 16bit)的字节数
static constexpr size_t kSampleBytes = CHANNELS * (BITS_PER_SAMPLE / 8);

// 喂进去的就是录音的原始采样率
static constexpr int kInputRate = SAMPLE_RATE;
static constexpr int kBitrate   = 24000;        // 单声道 24kbps

// 48k 采样就推一个页出去(libopusenc 默认 48000 = 1 秒)
static constexpr int kMuxingDelay = 4800;

COpusStreamWriter::~COpusStreamWriter(){
    Finish();                       // 兜底: 忘了 Finish 也要把尾巴编完
}

/**
 * @brief 给 libopusenc 的写回调: 一页数据到了, 加密后落盘
 * @param userData 用户数据指针, 指向 COpusStreamWriter 实例
 * @param p 输入数据指针
 * @param n 输入数据字节数
 * @return 0 成功 / 1 失败(libopusenc 的约定, 不是 errno)
 */
int COpusStreamWriter::OnWrite(void* userData, const unsigned char* p, opus_int32 n){
    COpusStreamWriter* self = static_cast<COpusStreamWriter*>(userData);
    if (!self || !p || n < 0)
        return 1;
    return self->Emit(p, static_cast<size_t>(n)) ? 0 : 1;
}

/**
 * @brief 关流回调。文件句柄归 CloseState 统一管, 这儿不重复关
 */
int COpusStreamWriter::OnClose(void* /*userData*/){
    return 0;
}

/**
 * @brief 释放编码器 + 关文件。跟 Open 里的创建顺序逆着来
 */
void COpusStreamWriter::CloseState(){
    if (m_enc){
        ope_encoder_destroy(m_enc);
        m_enc = nullptr;
    }

    if (m_file.is_open())
        m_file.close();
    m_bOpen = false;
}

/**
 * @brief 打开文件并写好头页
 * @param utf8Path 输出文件路径(UTF-8)
 * @param bEncrypt true 存成 .aopus 加密容器, false 存成明文 .opus
 * @return 操作状态
 */
AudioSdk::AudioSdkState COpusStreamWriter::Open(const char* utf8Path, bool bEncrypt){
    Finish();                       // 上一轮没关干净先收尾

    if (!utf8Path)
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;

    try {
        m_file.clear();
        m_file.open(std::filesystem::u8path(utf8Path),
                    std::ios::out | std::ios::binary | std::ios::trunc);
        if (!m_file.is_open())
            return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;   // 还没开成, 没什么可清的

        m_bEncrypt = bEncrypt;
        m_iWritten = 0;
        m_path     = utf8Path;

        if (bEncrypt) {             // .aopus 壳: 魔数 + 版本排在压缩流前面, 明文
            m_file.write(CEncryptedFormat::kAopusMagic, CEncryptedFormat::kAopusMagicSize);
            const uint16_t version = CEncryptedFormat::kAencVersion;
            m_file.write(reinterpret_cast<const char*>(&version), sizeof(version));
            if (!m_file.good()){
                CloseState();
                return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
            }
        }

        // 注释对象是必须的(它内部 stream_create 会直接解引用), 顺便署个名
        OggOpusComments* comments = ope_comments_create();
        if (!comments){
            CloseState();
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        }
        ope_comments_add(comments, "ENCODER", "audio_sdk");

        static const OpusEncCallbacks kCallbacks = {
            &COpusStreamWriter::OnWrite,
            &COpusStreamWriter::OnClose,
        };

        // 传的是输入采样率, 不是编码采样率 —— libopusenc 内部重采样到 48k
        int iErr = OPE_OK;
        m_enc = ope_encoder_create_callbacks(&kCallbacks, this, comments,
                                             kInputRate, CHANNELS, 0, &iErr);
        ope_comments_destroy(comments);   // create 里已经把内容拷走了
        if (!m_enc){
            AUDIO_LOG_ERROR(kTag, "libopusenc 初始化失败: %s", ope_strerror(iErr));
            CloseState();
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }

        ope_encoder_ctl(m_enc, OPUS_SET_BITRATE(kBitrate));
        ope_encoder_ctl(m_enc, OPE_SET_MUXING_DELAY(kMuxingDelay));

        // 头两个页立刻落盘: 录到一半崩了, 已有的数据也还是能播的
        iErr = ope_encoder_flush_header(m_enc);
        if (iErr != OPE_OK){
            AUDIO_LOG_ERROR(kTag, "写头页失败: %s", ope_strerror(iErr));
            CloseState();
            return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
        }
        m_file.flush();
        if (!m_file.good()){
            CloseState();
            return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
        }

        m_bOpen = true;
        return AudioSdk::AudioSdkState::NONE;
    }
    catch (const std::exception&){
        CloseState();
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
    }
}

/**
 * @brief 落一段字节(加密在这里做)
 * @param p 数据指针
 * @param n 要写字节数
 * @return 是否写成功
 */
bool COpusStreamWriter::Emit(const void* p, size_t n){
    if (!m_file.is_open())
        return false;
    if (n == 0)
        return true;                // 空 body 的页是合法的, 不是写失败

    const char* src = static_cast<const char*>(p);

    if (m_bEncrypt) {
        try {
            m_vecScratch.resize(n);
        } catch (const std::bad_alloc&) {
            return false;           // 音频线程上不能让异常穿出去
        }
        std::memcpy(m_vecScratch.data(), p, n);
        CEncryptedFormat::XorCrypt(m_vecScratch.data(), n, static_cast<size_t>(m_iWritten));
        src = reinterpret_cast<const char*>(m_vecScratch.data());
    }

    m_file.write(src, static_cast<std::streamsize>(n));
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
                        static_cast<unsigned long long>(m_iWritten), n, availMB);
        return false;
    }

    m_iWritten += n;
    return true;
}

/**
 * @brief 追加一块 PCM: 交给 libopusenc 攒帧、编码、分页; 页从回调里出来, 加密落盘
 * @param pcm 交错排列的 16bit PCM
 * @param n 字节数
 * @return NONE = 已收下; FILE_OPEN_FAILED = 没打开; FILE_WRITE_FAILED = 编码/写盘出错;
 *         INVALID_PARAMETER = 不是整采样点
 */
AudioSdk::AudioSdkState COpusStreamWriter::Write(void* pcm, size_t n){
    if (!m_bOpen)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
    if (n == 0)
        return AudioSdk::AudioSdkState::NONE;      // 空块不算失败(录音刚起可能给出 0 字节的块)
    if (!pcm || n % kSampleBytes != 0)
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;

    // 它要的是"每声道多少个采样", 而且多少都收 —— 攒帧是它内部的事
    const int iSamples = static_cast<int>(n / kSampleBytes);
    const int iErr = ope_encoder_write(m_enc,
                                       static_cast<const opus_int16*>(pcm), iSamples);
    if (iErr != OPE_OK){
        AUDIO_LOG_ERROR(kTag, "编码失败: %s", ope_strerror(iErr));
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
    }

    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 收尾: drain 出最后一帧 + EOS, 关文件
 * @return 落盘是否成功
 */
bool COpusStreamWriter::Finish(){
    if (!m_bOpen)
        return true;                // 没打开过、或已经收过尾 —— 没有残文件

    bool bSuccess = true;

    const int iErr = ope_encoder_drain(m_enc);
    if (iErr != OPE_OK){
        AUDIO_LOG_ERROR(kTag, "收尾失败: %s", ope_strerror(iErr));
        bSuccess = false;
    }
    ope_encoder_destroy(m_enc);     // drain 之后就该销毁, 它顺带触发 close 回调
    m_enc = nullptr;

    m_file.flush();
    bSuccess = m_file.good() && bSuccess;

    if (!bSuccess){
        CloseState();
        AUDIO_LOG_ERROR(kTag, "编码器落盘失败，路径: %s", m_path.c_str());
        return bSuccess;
    }
    AUDIO_LOG_ERROR(kTag, "编码器落盘成功，路径: %s", m_path.c_str());
    CloseState();
    return bSuccess;
}
