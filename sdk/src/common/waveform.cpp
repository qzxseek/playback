/* @Created On : 2026/9/15
   @Author : 孟源
   @note : 波形峰值计算实现(平台无关)
*/
#include "audio_sdk/waveform.h"

#include <cstring>      // std::memcpy

/**
 * @brief 从 16bit PCM 算峰峰值对(降采样), 归一化到 [-1, 1]
 * @param pcm16le   16bit 单声道 PCM 起始地址, 小端
 * @param byteCount 字节数(采样数 = byteCount / 2, 奇数尾字节忽略)
 * @param out       输出缓冲 [min0,max0,min1,max1,...], 共 2*points 个 float
 * @param points    峰值点个数
 */
void CWaveform::ComputePeaks(const void* pcm16le, size_t byteCount, float* out, int points)
{
    if (!pcm16le || !out || points <= 0 || byteCount < sizeof(int16_t))
        return;

    const uint8_t* bytes = static_cast<const uint8_t*>(pcm16le);
    const size_t sampleCount = byteCount / sizeof(int16_t);

    // 按字节读: 调用方的缓冲不保证 2 字节对齐, 逐采样 memcpy 是安全的读法
    // (编译器会把它优化成一条 load, 不会有额外开销)
    auto sampleAt = [bytes](size_t i) -> int16_t {
        int16_t v = 0;
        std::memcpy(&v, bytes + i * sizeof(int16_t), sizeof(int16_t));
        return v;
    };

    // 每点覆盖的采样数。至少 1: 数据比点数还少时, 后面的点会落到空区间,
    // 走下面 begin >= sampleCount 的分支补 0
    const size_t n = static_cast<size_t>(points);
    const size_t step = sampleCount / n > 0 ? sampleCount / n : 1;

    for (int i = 0; i < points; ++i)
    {
        const size_t begin = static_cast<size_t>(i) * step;

        // 最后一个点吃掉剩余全部采样, 否则 sampleCount 不是 points 整数倍时,
        // 尾部那截数据永远画不出来
        size_t end = (i == points - 1) ? sampleCount : begin + step;
        if (end > sampleCount) end = sampleCount;

        if (begin >= sampleCount)
        {
            // 点数比采样还多: 后面补 0, 不去重复算最后一段
            out[i * 2]     = 0.0f;
            out[i * 2 + 1] = 0.0f;
            continue;
        }

        int16_t mn = sampleAt(begin);
        int16_t mx = mn;
        for (size_t k = begin + 1; k < end; ++k)
        {
            const int16_t v = sampleAt(k);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }

        // 归一化: 除以 32768 —— -32768 恰好是 -1.0, 32767 是 0.99997, 不会越界
        out[i * 2]     = static_cast<float>(mn) / 32768.0f;
        out[i * 2 + 1] = static_cast<float>(mx) / 32768.0f;
    }
}
