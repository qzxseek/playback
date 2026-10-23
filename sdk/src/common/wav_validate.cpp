/* @Created On : 2026/8/11
   @Author : 孟源
   @note : WAV 文件校验类
*/
#include "audio_sdk/wav_validate.h"

#include <cstring>
#include <cstdint>

WavValidate::WavValidate() = default;
WavValidate::~WavValidate() = default;

/**
 * @brief 校验 WAV 文件头
 * @param data 缓冲数据
 * @param size 缓冲长度
 * @param fileSize 文件真实总长; 0 表示缓冲就是整个文件
 * @return 校验通过返回 true
 */
bool WavValidate::Validate(const uint8_t* data, size_t size, size_t fileSize){
    if (fileSize == 0) fileSize = size;      // 没给总长就是"缓冲即整文件"
    return ReadHeader(data, size) && ValidateHeader(data, size, fileSize);
}

/**
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
    m_dataOffset = sizeof(m_header);   // 每次校验重置, 由 ValidateHeader 修正
    return true;
}

/**
 * @brief 校验 WAV 文件头内容
 * @param data 原始文件缓冲(子块遍历要用 —— m_header 只是 44 字节拷贝,
 *             走到 LIST 这类子块时就出它的界了, 初版在这里踩过栈越界)
 * @param size 缓冲长度 
 * @param fileSize 文件真实总长度 —— 头里自报的长度一律跟它对
 * @return 校验通过返回 true
 */
bool WavValidate::ValidateHeader(const uint8_t* data, size_t size, size_t fileSize){
    // 顶层两标识: 容器必须是 RIFF/WAVE。fmt 在哪、data 在哪, 都交给下面的遍历。
    // (44 字节快照里的 fmtId/dataId/dataSize 只是"标准排布"的巧合快照,
    //  JUNK/bext 排在 fmt 前时那些偏移上全是别的块 —— 一律不读快照, 按遍历为准)
    if (std::memcmp(m_header.riffId, "RIFF", 4) != 0 ||
        std::memcmp(m_header.waveId, "WAVE", 4) != 0) {
        return false;
    }

    const uint8_t* const base = data;
    const size_t riffSize     = m_header.riffSize; // RIFF 块载荷长度 = 文件总长 - 8

    // riffSize 必须与文件实际大小一致: 短了是截断, 长了说明缓冲里混了别的东西。
    if (fileSize < 8 || riffSize != fileSize - 8)
        return false;

    // ---- 按 RIFF 子块遍历, 依次定位 fmt 块与 data 块的真实偏移 ----
    bool   haveFmt   = false;
    size_t pos       = 12;                         // 第一个子块紧跟 12 字节 RIFF 头
    while (size >= 8 && pos <= size - 8) {
        const char*     id   = reinterpret_cast<const char*>(base + pos);
        // 小端读出块长度
        const uint32_t  len  = static_cast<uint32_t>(base[pos + 4]) |
                               (static_cast<uint32_t>(base[pos + 5]) << 8) |
                               (static_cast<uint32_t>(base[pos + 6]) << 16) |
                               (static_cast<uint32_t>(base[pos + 7]) << 24);

        if (!haveFmt && std::memcmp(id, "fmt ", 4) == 0) {
            // 只认 PCM 的 16 字节 fmt: 载荷长度虚报或截断都拒
            if (len != 16 || size - pos - 8 < 16)
                return false;
            // fmt 载荷固定布局: 16 字节正好覆盖 audioFormat..bitsPerSample,
            // 从真实位置回填快照 —— 之后 Header() 给的就是真实参数
            std::memcpy(&m_header.audioFormat, base + pos + 8, 16);

            // 当前只支持标准 PCM WAV：音频格式编号为 1。
            // 声道数、采样率和位深不能为 0，位深必须是完整字节的倍数。
            if (m_header.audioFormat != 1 ||
                m_header.numChannels == 0 ||
                m_header.sampleRate == 0 ||
                m_header.bitsPerSample == 0 ||
                (m_header.bitsPerSample % 8) != 0) {
                return false;
            }

            // blockAlign/byteRate 必须与三个基本参数自洽
            // (blockAlign 在下面 data 的整帧检查里当除数, 0 或错值都会出乱子)
            const uint64_t expectedBlockAlign =
                static_cast<uint64_t>(m_header.numChannels) *
                (m_header.bitsPerSample / 8);
            const uint64_t expectedByteRate =
                m_header.sampleRate * expectedBlockAlign;
            if (m_header.blockAlign != expectedBlockAlign ||
                m_header.byteRate != expectedByteRate) {
                return false;
            }

            haveFmt = true;
        }
        else if (std::memcmp(id, "data", 4) == 0) {
            // data 先于 fmt: 没有格式参数, 不是能播的 WAV
            if (!haveFmt)
                return false;
            const size_t dataStart = pos + 8;       
            if (dataStart > fileSize || len > fileSize - dataStart)
                return false;
            // 数据长度必须是帧的整数倍, 否则最后一帧是残的
            if (len % m_header.blockAlign != 0)
                return false;
            m_dataOffset = pos + 8;
            m_dataSize   = len;
            return true;
        }

        const size_t nRemain = size - pos - 8;          // 循环条件已保证不下溢
        if (len > nRemain)
            break;                                      // 声明长度超出缓冲, 后面的块看不见
        pos += 8 + static_cast<size_t>(len) + (len & 1);
    }
    // 走到头也没找到合法的 fmt+data 组合 —— 不是能播的 WAV
    return false;
}
