/* @Created On : 2026/10/16
   @Author : 孟源
   @note : opus 编码实现(压缩格式层)
*/
#include "audio_sdk/audio_types.h"
#include "audio_sdk/opus_format.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/logging.h"
#include "audio_sdk/waveform.h"      // CWaveform::ComputePeaks(BuildPeaks 用)

#include <ogg/ogg.h>                 // 拆页(随 Vorbis 包一起给的 libogg)
#include <opus/opus.h>               // 解码器
#include <opus/opusenc.h>            // 源码在 tool/libopusenc

#include <algorithm>
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
        m_bBroken  = false;             // 新文件, 上一轮的写坏状态不带过来
        m_iWritten = 0;
        m_path     = utf8Path;

        if (bEncrypt) {             // .aopus 壳: 魔数 + 版本排在压缩流前面, 明文
            m_file.write(CEncryptedFormat::kAopusMagic, CEncryptedFormat::kAopusMagicSize);
            const uint16_t wVersion = CEncryptedFormat::kAencVersion;
            m_file.write(reinterpret_cast<const char*>(&wVersion), sizeof(wVersion));
            if (!m_file.good()){
                CloseState();
                return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
            }
        }

        // 注释对象是必须的(它内部 stream_create 会直接解引用), 顺便署个名
        OggOpusComments* pComments = ope_comments_create();
        if (!pComments){
            CloseState();
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        }
        ope_comments_add(pComments, "ENCODER", "audio_sdk");

        static const OpusEncCallbacks kCallbacks = {
            &COpusStreamWriter::OnWrite,
            &COpusStreamWriter::OnClose,
        };

        // 传的是输入采样率, 不是编码采样率 —— libopusenc 内部重采样到 48k
        int iErr = OPE_OK;
        m_enc = ope_encoder_create_callbacks(&kCallbacks, this, pComments,
                                             kInputRate, CHANNELS, 0, &iErr);
        ope_comments_destroy(pComments);  // create 里已经把内容拷走了
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
    if (m_bBroken)
        return false;               // 相位已经不可信, 收尾时 drain 也别再往盘上写
    if (!m_file.is_open())
        return false;
    if (n == 0)
        return true;                // 空 body 的页是合法的, 不是写失败

    const char* pSrc = static_cast<const char*>(p);

    if (m_bEncrypt) {
        try {
            m_vecScratch.resize(n);
        } catch (const std::bad_alloc&) {
            return false;           // 音频线程上不能让异常穿出去
        }
        std::memcpy(m_vecScratch.data(), p, n);
        CEncryptedFormat::XorCrypt(m_vecScratch.data(), n, static_cast<size_t>(m_iWritten));
        pSrc = reinterpret_cast<const char*>(m_vecScratch.data());
    }

    m_file.write(pSrc, static_cast<std::streamsize>(n));
    if (!m_file.good()) {
        m_bBroken = true;
        const int iErr = errno;
        std::error_code ecFs;
        const std::filesystem::space_info stSpace = std::filesystem::space(m_path, ecFs);
        const unsigned long long ullAvailMB =
            ecFs ? 0ULL : static_cast<unsigned long long>(stSpace.available / 1048576ULL);

        AUDIO_LOG_ERROR(kTag, "写失败 \"%s\": errno=%d%s%s, 已写 %llu 字节, 本次 %zu 字节, 剩余 %lluMB",
                        m_path.c_str(), iErr,
                        iErr == ENOSPC ? " [ENOSPC 磁盘空间不足]" : "",
                        ecFs ? " [拿不到剩余空间]" : "",
                        static_cast<unsigned long long>(m_iWritten), n, ullAvailMB);
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
    if (m_bBroken)
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;   // 已经写坏过, 别再用错相位往下写
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

    // 中途写坏过: 后面的页要么没落盘、要么相位错位, 这个文件已经不是合法容器了
    bool bSuccess = !m_bBroken;

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

// ==========解码器=========
namespace {

constexpr int kMaxFrame = 5760;          // opus 单包最多 120ms @48k

uint16_t ReadLE16(const uint8_t* p){
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}   // namespace

/**
 * @brief 判断是不是 AOPU 数据: 开头是 AOPU 魔数, 且 AOPU 魔数后面是 AOPU 数据
 */
bool COpusFormat::IsAopusData(const uint8_t* data, size_t size){
    return data && size >= CEncryptedFormat::kAopusMagicSize &&
           std::memcmp(data, CEncryptedFormat::kAopusMagic,
                       CEncryptedFormat::kAopusMagicSize) == 0;
}

/**
 * @brief 判断是不是明文 .opus: OggS 开头, 且第一页第一包是 OpusHead
 * @note 只看 OggS 不够 —— Vorbis 的 .ogg 也是 OggS 开头, 得再看第一个包叫什么。
 *       包在"页头 27 字节 + 段表 nSegs 字节"之后, 所以偏移是 27 + nSegs
 */
bool COpusFormat::IsOpusData(const uint8_t* data, size_t size){
    if (!data || size < 27 + 8)
        return false;
    if (std::memcmp(data, "OggS", 4) != 0 || data[4] != 0)
        return false;
    const size_t nSegs = data[26];
    if (27 + nSegs + 8 > size)
        return false;                    // 传进来的前缀太短, 判不了
    return std::memcmp(data + 27 + nSegs, kOpusHead, 8) == 0;
}

/**
 * @brief 解码器内部状态。ogg/opus 的头文件只在本 .cpp 里出现
 */
struct COpusStreamReader::Impl
{
    // 页索引: 扫一遍页头建的"目录", 只给 SeekTo 用(不用为了跳转从头解一遍)
    struct PageInfo {
        int64_t        llGp;      // 该页结束时的 granulepos(-1 = 没有整包结束)
        std::streamoff lOffset;   // 页首在载荷中的偏移(不含 AOPU 前缀)
    };

    std::ifstream    m_file;
    std::string      m_path;
    ogg_sync_state   m_sync{};    // 同步状态
    ogg_stream_state m_stream{};  // 流状态
    OpusDecoder*     m_pDec = nullptr;

    bool m_bSyncReady   = false;
    bool m_bStreamReady = false;
    bool m_bEncrypt     = false;
    bool m_bEos         = false;

    int    m_iSerial   = 0;
    size_t m_nPrefix   = 0;       // AOPU 壳长度, 明文是 0
    int    m_iChannels = 0;
    int    m_iPreSkip  = 0;
    int    m_iSkipPre  = 0;       // 开流后还要丢掉的 pre-skip 采样
    int64_t m_llTimeline     = 0; // 下一个吐出的采样在可播时间轴上的位置
    int64_t m_llDiscard      = 0; // 跳转后还要丢掉的采样
    int64_t m_llTotalSamples = 0; // gp - preSkip

    std::vector<PageInfo> m_vecPages;
    std::vector<int16_t>  m_vecPcm;   // 当前包解出的 PCM
    size_t m_nPcmPos = 0;
    // 解码途中遇到过坏包,Read 到底之后调用方靠它区分"正常播完"和"数据是残的"
    bool   m_bDecodeError = false;

    ~Impl(){
        Release();                // Open 中途失败时 delete p 也不会漏 ogg/opus 状态
    }

    void Release(){
        if (m_pDec){
            opus_decoder_destroy(m_pDec);
            m_pDec = nullptr;
        }
        if (m_bStreamReady){
            ogg_stream_clear(&m_stream);
            m_bStreamReady = false;
        }
        if (m_bSyncReady){
            ogg_sync_clear(&m_sync);
            m_bSyncReady = false;
        }
        if (m_file.is_open())
            m_file.close();
    }

    /**
     * @brief 从指定位置读 n 字节
     * @param lOff 位置(不含 AOPU 前缀)
     * @param pDst 目标缓冲
     * @param nBytes 最多读多少字节
     * @return 是否成功读 n 字节
     */
    bool ReadAt(std::streamoff lOff, void* pDst, size_t nBytes){
        m_file.clear();
        m_file.seekg(lOff);
        m_file.read(static_cast<char*>(pDst), static_cast<std::streamsize>(nBytes));
        return static_cast<size_t>(m_file.gcount()) == nBytes;
    }

    /**
     * @brief 从当前文件位置读一块喂给 libogg; 返回 false = 文件到底
     * @return 是否成功喂数据给 libogg
     * @note 这是解密唯一的挂载点: 文件里的字节在这里解成明文, 之后 libogg
     *       看到的都是干净的 Ogg。所以页头和页体不用分别解, 一次管全
     */
    bool FeedData(){
        char* pBuf = ogg_sync_buffer(&m_sync, 4096);  
        if (!pBuf)
            return false;
        // 位置要在 read 之前取: 末尾短读会把 failbit 置上, 之后 tellg() 返回 -1
        const std::streamoff lPos = m_file.tellg();
        m_file.read(pBuf, 4096);
        const std::streamsize lGot = m_file.gcount();
        if (lGot <= 0)
            return false;

        if (m_bEncrypt){
            if (lPos < 0)
                return false;
            const size_t nPhase = static_cast<size_t>(lPos - static_cast<std::streamoff>(m_nPrefix));
            CEncryptedFormat::XorCrypt(reinterpret_cast<uint8_t*>(pBuf),
                                       static_cast<size_t>(lGot), nPhase);
        }
        ogg_sync_wrote(&m_sync, static_cast<long>(lGot));
        return true;
    }
    
    /**
     * @brief 推进到下一整页, 交给 ogg_stream 拆包; 返回 false = 文件到底
     * @return 是否拿到一页
     * @note 循环有两层: pageout 没凑够一页(返回 0)就再读文件;
     *       凑够了但 serial 对不上(文件里混了别的流)就丢掉继续找
     */
    bool FeedPage(){
        for (;;){
            ogg_page ogPage;
            const int iRet = ogg_sync_pageout(&m_sync, &ogPage);
            if (iRet == 1){
                if (ogg_page_serialno(&ogPage) != m_iSerial)
                    continue;                       // 别的链, 跳过
                if (ogg_stream_pagein(&m_stream, &ogPage) != 0)
                    continue;
                if (ogg_page_eos(&ogPage))
                    m_bEos = true;
                return true;
            }
            if (iRet < 0)
                continue;                           // 坏数据, 丢掉接着找页头
            if (!FeedData())
                return false;
        }
    }
    
    /**
     * @brief 解下一包可播 PCM 进 m_vecPcm; >0 采样数, <=0 到底/出错
     * @return 采样数
     */
    int DecodeNext(){
        for (;;){
            ogg_packet ogPacket;
            // 从当前页里取包
            const int iRet = ogg_stream_packetout(&m_stream, &ogPacket);
            if (iRet < 0)
                continue;                           // 缺包, 跳过
            if (iRet == 0){
                if (m_bEos)
                    return -1;                      // EOS 页里的包已经取完
                if (!FeedPage())
                    return -1;
                continue;
            }

            if (ogPacket.bytes >= 8 &&
                (std::memcmp(ogPacket.packet, COpusFormat::kOpusHead, 8) == 0 ||
                 std::memcmp(ogPacket.packet, "OpusTags", 8) == 0))
                continue;                           // 头两包不是音频

            // ogPacket.packet 是压缩包(几十字节); aiPcm 收到的是解开的 PCM(2.5~120ms)
            int16_t aiPcm[kMaxFrame * 2];           // 只收 1/2 声道, Open 里已卡过
            const int iFrames = opus_decode(m_pDec, ogPacket.packet,
                                            static_cast<opus_int32>(ogPacket.bytes),
                                            aiPcm, kMaxFrame, 0);
            if (iFrames < 0){
                AUDIO_LOG_ERROR(kTag, "解码失败: %s", opus_strerror(iFrames));
                m_bDecodeError = true;
                return -1;
            }

            int iStart = 0;
            if (m_iSkipPre > 0){                    // 流开头的 pre-skip 要丢掉
                iStart = (std::min)(iFrames, m_iSkipPre);
                m_iSkipPre -= iStart;
            }
            int iAvail = iFrames - iStart;
            if (iAvail <= 0)
                continue;

            if (m_llDiscard > 0){                   // 跳转后多解出来的也丢掉
                const int iDrop = static_cast<int>((std::min)(static_cast<int64_t>(iAvail), m_llDiscard));
                m_llTimeline += iDrop;
                m_llDiscard  -= iDrop;
                iStart       += iDrop;
                iAvail       -= iDrop;
            }
            if (iAvail <= 0)
                continue;

            const int64_t llLeft = m_llTotalSamples - m_llTimeline;
            if (llLeft <= 0)
                return -1;
            if (iAvail > llLeft)
                iAvail = static_cast<int>(llLeft);  // 末帧补的静音不算

            // 必须当场拷走: ogPacket.packet 指向 libogg 内部缓冲, 下次取包就被覆盖。
            // 存在的意义是"余料" —— 包长和调用方一轮要的字节数不成倍数, 解多的下次先给
            const size_t nCount = static_cast<size_t>(iAvail) * m_iChannels;
            m_vecPcm.assign(aiPcm + static_cast<size_t>(iStart) * m_iChannels,
                            aiPcm + static_cast<size_t>(iStart) * m_iChannels + nCount);
            m_nPcmPos = 0;
            m_llTimeline += iAvail;
            return iAvail;
        }
    }
};

COpusStreamReader::COpusStreamReader() = default;

COpusStreamReader::~COpusStreamReader(){
    Close();
}

/**
 * @brief 打开 .opus / .aopus, 扫一遍页表拿到总时长和定位点
 * @param utf8Path 文件路径(UTF-8)
 * @return 操作状态
 */
AudioSdk::AudioSdkState COpusStreamReader::Open(const char* utf8Path){
    Close();

    if (!utf8Path)
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;

    Impl* pImpl = new (std::nothrow) Impl;
    if (!pImpl)
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
    pImpl->m_path = utf8Path;

    try {
        std::error_code ecFs;
        const auto ullFileSize = std::filesystem::file_size(std::filesystem::u8path(utf8Path), ecFs);
        if (ecFs || ullFileSize < 6){
            AUDIO_LOG_ERROR(kTag, "打不开或文件太小 path=\"%s\"", utf8Path);
            delete pImpl;
            return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
        }
        const std::streamoff lTotal = static_cast<std::streamoff>(ullFileSize);         

        pImpl->m_file.open(std::filesystem::u8path(utf8Path), std::ios::in | std::ios::binary);
        if (!pImpl->m_file.is_open()){
            delete pImpl;
            return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
        }

        // 取 512 是往上凑个整, 一次进内存的缓冲长度
        uint8_t abyHead[512] = {};
        const size_t nHeadLen = static_cast<size_t>((std::min)(lTotal, static_cast<std::streamoff>(sizeof(abyHead))));
        if (!pImpl->ReadAt(0, abyHead, nHeadLen)){
            AUDIO_LOG_ERROR(kTag, "读文件头失败 path=\"%s\"", utf8Path);
            delete pImpl;
            return AudioSdk::AudioSdkState::FILE_READ_FAILED;
        }
        if (COpusFormat::IsAopusData(abyHead, nHeadLen)){
            pImpl->m_bEncrypt = true;
            pImpl->m_nPrefix  = CEncryptedFormat::kAencPrefixSize;
        }
        else if (!COpusFormat::IsOpusData(abyHead, nHeadLen)){
            AUDIO_LOG_ERROR(kTag, "不是 Opus 容器 path=\"%s\"", utf8Path);
            delete pImpl;
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }

        // 扫页表: 每页记 (granulepos, 偏移), 顺带把第一页的 OpusHead 解出来。
        // 这一步不解码, 只读页头, 相当于给文件建个目录
        std::streamoff lOff = static_cast<std::streamoff>(pImpl->m_nPrefix);
        int64_t llLastGp = -1;
        while (lOff + 27 <= lTotal){
            // 页头固定 27 字节:
            //   OggS(4) 版本(1) flags(1) granulepos(8) serial(4) 页序(4) CRC(4) 段数(1)
            uint8_t abyHdr[27];
            if (!pImpl->ReadAt(lOff, abyHdr, sizeof(abyHdr)))
                break;
            // .aopus 从前缀之后就整段 XOR, 页头也是密文; 扫描时按载荷内偏移解一下
            if (pImpl->m_bEncrypt)
                CEncryptedFormat::XorCrypt(abyHdr, sizeof(abyHdr),
                                           static_cast<size_t>(lOff - static_cast<std::streamoff>(pImpl->m_nPrefix)));
            // 对不上页头就是走偏了(多半文件被截断), 停下, 前面扫到的照样能用
            if (std::memcmp(abyHdr, "OggS", 4) != 0)
                break;
            const uint8_t byFlags = abyHdr[5];       // 0x04 = EOS
            uint64_t ullGp = 0;                  // granulepos 小端 64 位
            for (int i = 7; i >= 0; --i)
                ullGp = (ullGp << 8) | abyHdr[6 + i];
            const int64_t llGp = static_cast<int64_t>(ullGp);
            const size_t nSegs = abyHdr[26];
            if (lOff + 27 + static_cast<std::streamoff>(nSegs) > lTotal)
                break;

            // 段表: 每项是"下一段多长"(0~255)。整页 = 27 + 段数 + 段长之和, 没有固定大小
            uint8_t abySegs[255];
            if (!pImpl->ReadAt(lOff + 27, abySegs, nSegs))
                break;
            if (pImpl->m_bEncrypt)
                CEncryptedFormat::XorCrypt(abySegs, nSegs,
                                           static_cast<size_t>(lOff + 27 - static_cast<std::streamoff>(pImpl->m_nPrefix)));
            size_t nBody = 0;
            for (size_t i = 0; i < nSegs; ++i)
                nBody += abySegs[i];
            // 检查每一页大小是否超出文件大小
            if (lOff + 27 + static_cast<std::streamoff>(nSegs) + static_cast<std::streamoff>(nBody) > lTotal)
                break;

            const std::streamoff lBodyOff = lOff + 27 + static_cast<std::streamoff>(nSegs);
            if (pImpl->m_vecPages.empty()){
                // 第一页第一包: OpusHead
                if (nSegs == 0 || abySegs[0] < 19 || abySegs[0] > nBody){
                    break;
                }
                uint8_t abyOpusHead[19];
                if (!pImpl->ReadAt(lBodyOff, abyOpusHead, sizeof(abyOpusHead)))
                    break;
                if (pImpl->m_bEncrypt)
                    CEncryptedFormat::XorCrypt(abyOpusHead, sizeof(abyOpusHead),
                                               static_cast<size_t>(lBodyOff - static_cast<std::streamoff>(pImpl->m_nPrefix)));
                if (std::memcmp(abyOpusHead, COpusFormat::kOpusHead, 8) != 0)
                    break;
                // 只取需要的四个字段, 其余(版本/原始采样率/增益)用不上
                const int iFamily = abyOpusHead[18];
                pImpl->m_iChannels = abyOpusHead[9];
                pImpl->m_iPreSkip  = ReadLE16(abyOpusHead + 10);
                // serial 不在 OpusHead 里, 在页头; 后面 ogg_stream_init 靠它认流
                pImpl->m_iSerial   = abyHdr[14] | (abyHdr[15] << 8) | (abyHdr[16] << 16) | (abyHdr[17] << 24);
                if (pImpl->m_iChannels < 1 || pImpl->m_iChannels > 2 || iFamily != 0){
                    AUDIO_LOG_ERROR(kTag, "只支持单/双声道映射(family=%d, ch=%d)", iFamily, pImpl->m_iChannels);
                    delete pImpl;
                    return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
                }
            }

            pImpl->m_vecPages.push_back({ llGp, lOff - static_cast<std::streamoff>(pImpl->m_nPrefix) });
            if (llGp >= 0)
                llLastGp = llGp;

            lOff += 27 + static_cast<std::streamoff>(nSegs) + static_cast<std::streamoff>(nBody);
            if (byFlags & 0x04)                 // EOS
                break;
        }

        if (pImpl->m_iChannels == 0 || pImpl->m_vecPages.empty()){
            AUDIO_LOG_ERROR(kTag, "Ogg 页表扫不出来 path=\"%s\"", utf8Path);
            delete pImpl;
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }

        // 时长只能靠末页的 granulepos: Opus 是变长帧, 从文件大小推不出来。
        // gp 含开头的 pre-skip, 所以可播采样数要减掉它
        constexpr int64_t kMaxSamples = int64_t{1} << 40;
        if (llLastGp < 0 || llLastGp > kMaxSamples){
            AUDIO_LOG_ERROR(kTag, "granulepos 超出合理范围(%lld) path=\"%s\" -> FORMAT_NOT_SUPPORTED",
                            static_cast<long long>(llLastGp), utf8Path);
            delete pImpl;
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        pImpl->m_llTotalSamples = llLastGp > pImpl->m_iPreSkip ? llLastGp - pImpl->m_iPreSkip : 0;

        int iErr = OPUS_OK;
        pImpl->m_pDec = opus_decoder_create(kRate, pImpl->m_iChannels, &iErr);
        if (!pImpl->m_pDec || iErr != OPUS_OK){
            AUDIO_LOG_ERROR(kTag, "opus 解码器创建失败: %s", opus_strerror(iErr));
            delete pImpl;
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        ogg_sync_init(&pImpl->m_sync);
        pImpl->m_bSyncReady = true;
        if (ogg_stream_init(&pImpl->m_stream, pImpl->m_iSerial) != 0){
            delete pImpl;
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        }
        pImpl->m_bStreamReady = true;

        // 扫页表时游标已经走到文件尾了, 拨回开头, 后面的 Read 从第一页重新解
        pImpl->m_file.clear();
        pImpl->m_file.seekg(static_cast<std::streamoff>(pImpl->m_nPrefix));
        pImpl->m_iSkipPre   = pImpl->m_iPreSkip;   // 第一包解出来先丢 pre-skip 个采样
        pImpl->m_llTimeline = 0;
        pImpl->m_bDecodeError = false;             // 新一轮, 清掉上一次的
        pImpl->m_llDiscard  = 0;

        m_impl       = pImpl;
        m_bOpen      = true;
        m_iChannels  = pImpl->m_iChannels;
        m_iBlockAlign = static_cast<uint16_t>(pImpl->m_iChannels * (BITS_PER_SAMPLE / 8));
        m_nTotalBytes = static_cast<size_t>(pImpl->m_llTotalSamples) * m_iBlockAlign;
        return AudioSdk::AudioSdkState::NONE;
    }
    catch (const std::exception&){
        delete pImpl;
        return AudioSdk::AudioSdkState::FILE_READ_FAILED;
    }
}

void COpusStreamReader::Close(){
    if (m_impl){
        m_impl->Release();
        delete m_impl;
        m_impl = nullptr;
    }
    m_bOpen      = false;
    m_iChannels  = 0;
    m_iBlockAlign = 0;
    m_nTotalBytes = 0;
}

/**
 * @brief 读交错 16bit PCM
 * @param dst 目标缓冲
 * @param n 最多读多少字节(不足一个采样点的尾巴会截掉)
 * @return 实际读到的字节数, 0 = 到底
 */
size_t COpusStreamReader::Read(uint8_t* dst, size_t n){
    if (!m_bOpen || !m_impl || !dst || m_iBlockAlign == 0)
        return 0;

    Impl& impl = *m_impl;
    n -= n % m_iBlockAlign;

    // 两段循环: 先吃掉上次解多剩下的, 吃完了再解下一包
    size_t nDone = 0;
    while (nDone < n){
        if (impl.m_nPcmPos < impl.m_vecPcm.size()){
            const size_t nHave = (impl.m_vecPcm.size() - impl.m_nPcmPos) * sizeof(int16_t);
            const size_t nTake = (std::min)(nHave, n - nDone);
            std::memcpy(dst + nDone, impl.m_vecPcm.data() + impl.m_nPcmPos, nTake);
            impl.m_nPcmPos += nTake / sizeof(int16_t);
            nDone += nTake;
            continue;
        }
        if (impl.DecodeNext() <= 0)
            break;
    }
    return nDone;
}

bool COpusStreamReader::HadDecodeError() const{
    return m_impl && m_impl->m_bDecodeError;
}

/**
 * @brief 按 PCM 字节位置定位。Opus 是变长帧, 只能退到最近的页边界再丢样点
 * @param pcmBytes 目标位置(相对整条流开头)
 * @return 操作状态
 */
AudioSdk::AudioSdkState COpusStreamReader::SeekTo(size_t pcmBytes){
    if (!m_bOpen || !m_impl)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    Impl& impl = *m_impl;
    int64_t llTarget = static_cast<int64_t>(pcmBytes / m_iBlockAlign);
    if (llTarget > impl.m_llTotalSamples)
        llTarget = impl.m_llTotalSamples;
    if (llTarget < 0)
        llTarget = 0;

    // 最后一个 granulepos 不超过目标的页
    size_t nIdx = impl.m_vecPages.size();
    for (size_t i = 0; i < impl.m_vecPages.size(); ++i){
        if (impl.m_vecPages[i].llGp < 0)
            continue;
        if (impl.m_vecPages[i].llGp - impl.m_iPreSkip <= llTarget)
            nIdx = i;
        else
            break;
    }

    std::streamoff lOff  = 0;
    int64_t        llBase = 0;
    // 起点是否还落在 pre-skip 区里
    bool bAtHead = (nIdx == 0);                  // 第 0 页之前没有页 = 流头
    if (nIdx < impl.m_vecPages.size()){
        lOff = impl.m_vecPages[nIdx].lOffset;
        for (size_t j = nIdx; j-- > 0;){         // 该页之前的 granulepos = 起点
            if (impl.m_vecPages[j].llGp >= 0){
                llBase = impl.m_vecPages[j].llGp - impl.m_iPreSkip;
                if (llBase < 0){
                    bAtHead = true;              // 前一页还在 pre-skip 区内
                    llBase  = 0;
                }
                break;
            }
        }
    }

    // libogg 和解码器都还记着跳转前的位置, 必须一起复位, 否则会从半截包开始解
    impl.m_file.clear();
    impl.m_file.seekg(static_cast<std::streamoff>(impl.m_nPrefix) + lOff);
    if (!impl.m_file)
        return AudioSdk::AudioSdkState::FILE_READ_FAILED;

    ogg_sync_reset(&impl.m_sync);
    ogg_stream_reset(&impl.m_stream);
    opus_decoder_ctl(impl.m_pDec, OPUS_RESET_STATE);
    impl.m_bEos = false;
    impl.m_bDecodeError = false;      // 跳到新位置, 上一段的解码错误不算在这一段
    impl.m_vecPcm.clear();
    impl.m_nPcmPos = 0;

    impl.m_iSkipPre   = bAtHead ? impl.m_iPreSkip : 0;
    impl.m_llTimeline = llBase;
    impl.m_llDiscard  = llTarget - llBase;
    if (impl.m_llDiscard < 0)
        impl.m_llDiscard = 0;
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 整条解一遍, 把 PCM 降采样成 points 个峰值点
 * @param out      输出缓冲, 装得下 points*2 个 float
 * @param points   峰值点个数
 * @param cb       进度回调(可传 NULL); 每次给的 points 参数是【已填了几个点】
 * @param userData 原样透传给回调
 * @return 解完返回 NONE; 没打开过返回 FILE_OPEN_FAILED
 */
AudioSdk::AudioSdkState COpusStreamReader::BuildPeaks(float* out, int points,
                                                      AudioSdkWaveCallback cb, void* userData){
    if (!m_bOpen || !out || points <= 0)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    const size_t nPoints = static_cast<size_t>(points);
    for (size_t i = 0; i < nPoints * 2; ++i)
        out[i] = 0.0f;

    size_t nSamples = m_nTotalBytes / sizeof(int16_t) / nPoints;
    if (nSamples == 0)
        nSamples = 1;
    const size_t nStepBytes   = nSamples * sizeof(int16_t);
    const size_t nReportEvery = nPoints > 32 ? nPoints / 32 : 1;

    std::vector<uint8_t> vecBuf(64 * 1024);
    size_t nConsumed = 0;
    size_t nFilled   = 0;
    size_t nReported = 0;

    for (size_t i = 0; i < nPoints && nConsumed < m_nTotalBytes; ++i){
        const size_t nEnd = (i + 1 == nPoints)
                                ? m_nTotalBytes
                                : (std::min)((i + 1) * nStepBytes, m_nTotalBytes);

        float fMin = 0.0f, fMax = 0.0f;
        bool  bAny = false;
        while (nConsumed < nEnd){
            const size_t nWant = (std::min)(vecBuf.size(), nEnd - nConsumed);
            const size_t nGot  = Read(vecBuf.data(), nWant);
            if (nGot == 0)
                break;

            float afMM[2] = {};
            CWaveform::ComputePeaks(vecBuf.data(), nGot, afMM, 1);
            if (!bAny || afMM[0] < fMin) fMin = afMM[0];
            if (!bAny || afMM[1] > fMax) fMax = afMM[1];
            bAny = true;
            nConsumed += nGot;
        }

        if (bAny){
            out[i * 2]     = fMin;
            out[i * 2 + 1] = fMax;
            nFilled = i + 1;
        }

        if (cb && nFilled - nReported >= nReportEvery){
            nReported = nFilled;
            cb(out, static_cast<int>(nFilled), userData);
        }
    }

    if (cb && nReported != nFilled)
        cb(out, static_cast<int>(nFilled), userData);
    return AudioSdk::AudioSdkState::NONE;
}
