/* @Created On : 2026/9/15
   @Author : 孟源
   @note : 波形峰值计算(平台无关)。
          画波形不可能把采样全画出来 —— 44100Hz 下 100ms 就是 4410 个采样,
          而窗口宽度才几百像素。所以按"每个像素列取该段的最大/最小值"降采样,
          这也是 Audacity 等音频软件的做法。

          本文件只做计算, 不碰设备、不碰线程, Windows 与 Android 共用同一份。
*/
#pragma once

#include "audio_sdk/audio_types.h"

#include <cstddef>
#include <cstdint>

class CWaveform
{
public:
    // 录音: 每块(100ms)输出的峰值点数。
    // 一块 4410 个采样降到 256 点, 约每 17 个采样一个点, 够画出细节。
    // 数值定义在 audio_types.h —— 调用方要按它开接收缓冲, 属于接口契约。
    static constexpr int kPointsPerBlock = AUDIO_SDK_WAVE_BLOCK_POINTS;

    // 播放: 整个文件的峰值点数。UI 宽度约 900px, 1024 点足够铺满。
    static constexpr int kFilePoints = AUDIO_SDK_WAVE_FILE_POINTS;

    /**
     * @brief 从 16bit PCM 算峰峰值对(降采样), 归一化到 [-1, 1]
     * @param pcm16le    16bit 单声道 PCM 起始地址, 小端
     * @param byteCount  字节数(采样数 = byteCount / 2, 奇数尾字节忽略)
     * @param out        输出缓冲, 布局 [min0, max0, min1, max1, ...], 共 2*points 个 float
     * @param points     要输出多少个峰值点(<= 0 或数据为空则不做)
     * @note 收字节指针而不是 int16_t*, 是因为调用方的缓冲(std::vector<uint8_t>、
     *       设备回调的 char[])并不保证 2 字节对齐, 直接转型访问是未定义行为;
     *       这里内部逐采样 memcpy, 由编译器优化成单条 load, 对任意对齐都安全。
     *       每点覆盖 sampleCount/points 个采样; 尾部不足一点的数据会并入最后一个点,
     *       保证所有采样都被算进去(不丢尾部数据)。
     */
    static void ComputePeaks(const void* pcm16le, size_t byteCount, float* out, int points);
};
