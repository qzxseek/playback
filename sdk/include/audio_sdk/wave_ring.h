/* @Created On : 2026/9/22
   @Author : 孟源
   @note : 录音波形的环形缓冲(平台无关) —— 单生产者单消费者, 不加锁。

          线程约定:
            音频线程  只调 Push      —— 逐点拷贝 + 一个 release store, 不分配、不加锁。
            调用线程  只调 Pop/Reset —— 只读快照, 同样不加锁。
          两条线程各动各的游标, 唯一的共享变量是 m_total, 靠 acquire/release 配对同步。

          Windows 与 Android 共用同一份: 这类无锁代码最怕两端各写一遍后走样。
*/
#pragma once

#include "audio_sdk/audio_types.h"

#include <atomic>

class CWaveRing{
public:
    
    void Push(const float* minmax, int points);

    int Pop(float* outMinMax, int maxPoints);


    void Reset();

private:
    static constexpr unsigned kCapacity = AUDIO_SDK_WAVE_RING_POINTS;

    float m_slot[kCapacity][2] = {};    // [i][0]=min, [i][1]=max
    std::atomic<unsigned> m_total{0};   // 音频线程写: 累计推入的点数(单调递增)
    unsigned m_read = 0;                // 调用线程写: 累计取走的点数(单调递增)
};
