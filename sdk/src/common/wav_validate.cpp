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
 * @brief 校验内存中的 WAV 文件
 * @param data 整个 WAV 文件的内存缓冲（文件头 + PCM 数据）
 * @param size 缓冲长度
 * @return 校验通过返回 true
 */
bool WavValidate::Validate(const uint8_t* data, size_t size){
    return ReadHeader(data, size) && ValidateHeader(data, size);
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
 * @param size 文件实际总长度
 * @return 校验通过返回 true
 */
bool WavValidate::ValidateHeader(const uint8_t* data, size_t size){
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
    if (riffSize + 8u != size)
        return false;

    // ---- 按 RIFF 子块遍历, 依次定位 fmt 块与 data 块的真实偏移 ----
    // 规范只保证 fmt 在 data 之前, 不保证它紧跟 "WAVE":
    // JUNK(填充预留)、bext(广播波) 可以排在 fmt 前面, LIST/INFO 等排在 fmt 与 data 之间。
    // 每个块 = 4 字节块 ID + 4 字节小端长度 + 载荷; 块之间按 2 字节对齐
    // 遍历必须走【原缓冲】: m_header 只是 44 字节的拷贝, 块的位置可以远在它之外
    bool   haveFmt   = false;
    size_t pos       = 12;                         // 第一个子块紧跟 12 字节 RIFF 头
    while (pos + 8 <= size) {
        const char*     id   = reinterpret_cast<const char*>(base + pos);
        // 小端读出块长度
        const uint32_t  len  = static_cast<uint32_t>(base[pos + 4]) |
                               (static_cast<uint32_t>(base[pos + 5]) << 8) |
                               (static_cast<uint32_t>(base[pos + 6]) << 16) |
                               (static_cast<uint32_t>(base[pos + 7]) << 24);

        if (!haveFmt && std::memcmp(id, "fmt ", 4) == 0) {
            // 只认 PCM 的 16 字节 fmt: 载荷长度虚报或截断都拒
            if (len != 16 || pos + 8 + 16 > size)
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
            const uint32_t expectedBlockAlign =
                static_cast<uint32_t>(m_header.numChannels) *
                (m_header.bitsPerSample / 8);
            const uint32_t expectedByteRate =
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
            // 不能超过文件末尾(data 后允许再垫别的子块, 不要求顶到 EOF)
            if (len + (pos + 8) > size)
                return false;
            // 数据长度必须是帧的整数倍, 否则最后一帧是残的
            if (len % m_header.blockAlign != 0)
                return false;
            m_dataOffset = pos + 8;
            m_dataSize   = len;
            return true;
        }

        // 其它子块: 跳过载荷 + 奇数长度时的 1 字节填充
        pos += 8 + len + (len & 1);
    }
    // 走到头也没找到合法的 fmt+data 组合 —— 不是能播的 WAV
    return false;
}
