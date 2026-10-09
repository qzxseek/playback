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
    // 录音: 每块(时长见 AUDIO_SDK_BLOCK_MS)输出的峰值点数。
    // 按默认 100ms 一块算, 4410 个采样降到 256 点, 约每 17 个采样一个点, 够画出细节。
    // 数值不随块时长变(变的是回调频率, 不是每次给的点数); 定义在 audio_types.h
    // —— 调用方要按它开接收缓冲, 属于接口契约。
    static constexpr int kPointsPerBlock = AUDIO_SDK_WAVE_BLOCK_POINTS;

    // 播放: 整个文件的峰值点数。UI 宽度约 900px, 1024 点足够铺满。
    static constexpr int kFilePoints = AUDIO_SDK_WAVE_FILE_POINTS;

    static void ComputePeaks(const void* pcm16le, size_t byteCount, float* out, int points);

    static bool BuildFilePeaks(const char* utf8Path, size_t payloadOffset, size_t dataSize,
                               bool bEncrypted, float* out, int points);
};
