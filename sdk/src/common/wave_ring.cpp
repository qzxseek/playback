#include "audio_sdk/wave_ring.h"
/**
 * @brief 推入一批峰值点。minmax 布局 [min0,max0,min1,max1,...], 共 points*2 个 float。
 * @param minmax 峰值点缓冲, 布局 [min0,max0,min1,max1,...], 共 points*2 个 float
 * @param points 要输入多少个峰值点(<= 0 或数据为空则不做)
 */
void CWaveRing::Push(const float* minmax, int points){
        if (!minmax || points <= 0) return;

        // 一次推来的比整个环还大: 只留最后 kCapacity 个。
        // (下标本来就取模, 不这样处理也不会越界, 但前面的点会被自己盖掉, 白拷一遍)
        if (points > static_cast<int>(kCapacity)){
            minmax += (points - static_cast<int>(kCapacity)) * 2;
            points  = static_cast<int>(kCapacity);
        }

        unsigned total = m_total.load(std::memory_order_relaxed);
        for (int i = 0; i < points; ++i){
            float* slot = m_slot[total % kCapacity];
            slot[0] = minmax[i * 2];        // min
            slot[1] = minmax[i * 2 + 1];    // max
            ++total;
        }
        // release: 保证上面写进槽里的数据, 对取到这个总数的消费者可见
        m_total.store(total, std::memory_order_release);
    }

/**
    * @brief 从环中取一批峰值点。outMinMax 布局 [min0,max0,min1,max1,...], 共 points*2 个 float。
    * @param outMinMax 峰值点缓冲, 布局 [min0,max0,min1,max1,...], 共 points*2 个 float
    * @param maxPoints 要取多少个峰值点(<= 0 或数据为空则不做)
    * @return 实际取到的峰值点数
    */
int CWaveRing::Pop(float* outMinMax, int maxPoints){
    if (!outMinMax || maxPoints <= 0) return 0;

    const unsigned total = m_total.load(std::memory_order_acquire);
    // 两支游标都是单调递增的, 差值天然就是"没读过的点数"。
    // 这里不用取模游标: (写-读+容量)%容量 在写方正好绕满一圈时得到 0,
    // 会把"环满"误判成"无数据"; 单调计数没有这个歧义。
    unsigned avail = total - m_read;
    if (avail == 0) return 0;

    // 写方超过一整圈(调用方太久没取): 只给最新的 kCapacity 个, 更老的丢掉
    if (avail > kCapacity){
        m_read = total - kCapacity;
        avail  = kCapacity;
    }
    if (avail > static_cast<unsigned>(maxPoints)) avail = static_cast<unsigned>(maxPoints);

    for (unsigned i = 0; i < avail; ++i){
        const float* slot = m_slot[m_read % kCapacity];
        outMinMax[i * 2]     = slot[0];
        outMinMax[i * 2 + 1] = slot[1];
        ++m_read;
    }
    return static_cast<int>(avail);
}

/**
 * @brief 开新一轮: 两支游标归零, 上一轮没取走的点一并丢弃。
 * 【调用前音频线程必须没在跑】—— 录音器只在 StartRecording 里调, 那时设备还没启动。
 */
void CWaveRing::Reset(){
        m_read = 0;
        m_total.store(0, std::memory_order_relaxed);
    }
