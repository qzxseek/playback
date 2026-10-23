/* @Created On : 2026/10/12
   @Author : 孟源
   @note : ogg/vorbis 编解码接口(压缩格式层)
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "audio_sdk/audio_types.h"
#include "audio_sdk/encrypted_format.h"   // kAoggMagic: 加密魔数跟 kAencMagic 放一起

#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

/**
 * @brief Ogg 容器标识
 */
class COggFormat
{
public:
    static constexpr char   kOggMagic[4] = {'O', 'g', 'g', 'S'};   // 明文
    static constexpr size_t kMagicSize   = 4;

    // 只比魔数, 不校验内部结构 —— 跟 CEncryptedFormat::IsAencData 一个粒度
    static bool IsOggData(const uint8_t* data, size_t size);
    static bool IsAoggData(const uint8_t* data, size_t size);
};

/**
 * @brief 边录边编码落盘器: 开文件先写头三个页 → 逐块编码成页追加 → 收尾排空
 */
class COggStreamWriter
{
public:
    COggStreamWriter() = default;
    ~COggStreamWriter();                 

    AudioSdk::AudioSdkState Open(const char* utf8Path, bool bEncrypt);
    AudioSdk::AudioSdkState Write(void* pcm, size_t n);
    bool Finish();

    uint64_t GetWritten() const { return m_iWritten; }   

private:
    // 把编码管道推干净: 分析 → 分块 → 定型 → 打包 → 分页。各层含义见 .cpp
    bool Drain();
    // 落一段 Ogg 字节(加密在这里做)。返回 false = 写失败
    bool Emit(const void* p, size_t n);
    void CloseState();

    std::ofstream m_file;
    std::vector<uint8_t> m_vecScratch;      // 加密暂存, 见 Emit 的说明
    std::string m_path;            // 落盘路径: 写失败时用它查剩余空间 + 记日志
    bool m_bOpen = false;           // 编码器可用(五个结构体已 init)
    bool m_bBroken = false;         // 判断落盘是否写坏过一次
    bool m_bEncrypt = false;
    uint64_t m_iWritten = 0;    // 已编码的载荷字节数, 也是下一段的 XOR 相位

    // 编码器状态
    vorbis_info      m_vi{};            // 编码器参数(采样率/声码率)
    vorbis_comment   m_vc{};            // 元数据(编码器标识)
    vorbis_dsp_state m_vd{};            // 分析状态(MDCT 缓冲和重叠窗口)
    vorbis_block     m_vb{};            // 一次编码用的工作块
    ogg_stream_state m_os{};            // 分页状态(攒包、算 CRC、切页)
};

/**
 * @brief Ogg 解码读取器: 按 PCM 字节偏移取数据, 顺序读或跳读
 */
class COggStreamReader
{
public:
    COggStreamReader() = default;
    ~COggStreamReader();                 // 兜底: 忘了 Close 也要 ov_clear

    // 自己开文件并识别前缀(.aogg 自动走解密)。传进来的必须是已落盘的路径。
    AudioSdk::AudioSdkState Open(const char* utf8Path);
    void Close();

    bool IsOpen() const { return m_open; }

    // 解码出来的格式 —— 播放器拿它填 WAVEFORMATEX
    int32_t  GetChannels() const { return m_channels; }
    int32_t  GetSampleRate() const { return m_rate; }
    uint16_t GetBlockAlign() const { return m_blockAlign; }       // 每帧字节数
    uint16_t GetBitsPerSample() const { return BITS_PER_SAMPLE; }

    size_t GetTotalBytes() const { return m_totalBytes; }
    size_t Read(uint8_t* dst, size_t n);
    AudioSdk::AudioSdkState SeekTo(size_t pcmBytes);

    /**
     * @brief 整条解一遍, 把 PCM 降采样成 points 个峰值点
     * @param out      输出缓冲, 装得下 points*2 个 float
     * @param points   峰值点个数
     * @param cb       进度回调(可传 NULL)。每积够一批就报一次, points 参数给的是
     *                 【已填了几个点】—— 从少到多, 调用方据此画"从左往右长出来"
     * @param userData 原样透传给回调
     * @return 解完返回 NONE; 没打开过返回 FILE_OPEN_FAILED
     * @note 这是只读扫描, 跑完就把 read 位置留在末尾 —— 别拿正在播的实例调它。
     *       PCM 解出一块就扔, 内存只有一个 64KB 缓冲, 不随文件时长涨。
     */
    AudioSdk::AudioSdkState BuildPeaks(float* out, int points,
                                       AudioSdkWaveCallback cb, void* userData);

private:
    // ov_callbacks 的四个静态入口 —— 作用只是把 datasource 转回 this
    static size_t ReadCb(void* ptr, size_t size, size_t nmemb, void* ds);
    static int    SeekCb(void* ds, ogg_int64_t offset, int whence);
    static int    CloseCb(void* ds);          // 空实现: 文件归这个类自己管
    static long   TellCb(void* ds);

    std::ifstream  m_file;
    OggVorbis_File m_vf{};
    bool m_open    = false;
    bool m_encrypt = false;
    size_t m_prefix  = 0;        // 文件开头要跳过的字节数: .aogg 是 6, 明文是 0
    size_t m_payload = 0;        // 载荷总字节数(文件大小 - m_prefix), 供 SEEK_END

    // 参数
    int32_t  m_channels   = 0;
    int32_t  m_rate       = 0;
    uint16_t m_blockAlign = 0;
    size_t   m_totalBytes = 0;
};
