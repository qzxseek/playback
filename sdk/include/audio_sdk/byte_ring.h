/* @Created On : 2026/10/8
   @Author : 孟源
   @note : 按字节的环形缓冲(平台无关) —— 单生产者单消费者, 不加锁。

          和 wave_ring.h 是同一个路子, 但传的东西不一样:
            波形环 传"降采样后的峰值点" —— 丢了只是画面糙一点;
            字节环 传"马上要送去设备的 PCM" —— 丢了就是能听见的爆音。
          所以这里的读写一律按【绝对值】寻址: 那个值是数据在文件里的偏移, 不是
          一个绕圈的序数。好处是两边都能直接说"我要第 N 字节", 不用先换算成环内
          下标 —— 随机访问(Seek)因此只是一次 Reset, 不需要额外的握手协议。

          线程约定:
            生产者(读文件那条线程)  只调 Head/Base/Space/Write/Reset
            消费者(音频回调)        只调 Tail/ReadAt/AdvanceTail
            Seek 由调用方线程调      SeekTo
          m_tail 会被两支线程写(消费者 CAS 推进, SeekTo 直接落值), 这与原来
          m_readPos 的用法完全一致: Seek 期间那次 CAS 会失败, 消费者放弃这一块,
          下一轮从新位置重新来。

          【读越界的保证】下标一律按容量取模, 所以哪怕游标正被 Reset 拽走,
          读到的也只是"过时的字节", 永远不会读到缓冲之外。这不是巧合, 是这里
          刻意选的寻址方式 —— 实时线程上宁可放一声杂音, 不能踩内存。
*/
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class CByteRing{
public:
    // 默认容量 256KB ≈ 3 秒(44.1kHz/16bit 单声道)。够生产者从容地按块补货,
    // 又远小于任何一段录音, 内存不随文件长度涨。
    static constexpr size_t kDefaultCapacity = 256 * 1024;

    // 容量会被向上取到 2 的幂(取模要点成按位与)
    explicit CByteRing(size_t capacityBytes = kDefaultCapacity);
    ~CByteRing();

    // —— 生产者 ——
    size_t Head() const { return m_head.load(std::memory_order_acquire); }
    size_t Base() const { return m_base.load(std::memory_order_acquire); }
    size_t Space() const;
    size_t Write(const uint8_t* src, size_t n);
    void Reset(size_t pos);

    // —— 消费者 ——
    size_t Tail() const { return m_tail.load(std::memory_order_relaxed); }
    void ReadAt(size_t pos, uint8_t* dst, size_t n) const;
    bool AdvanceTail(size_t expect, size_t next);
    void SeekTo(size_t pos) { m_tail.store(pos, std::memory_order_release); }

private:
    std::vector<uint8_t> m_buf;
    size_t m_mask = 0;                  // 容量 - 1
    std::atomic<size_t> m_base{0};      // 生产者写: 环内有效数据的最老位置
    std::atomic<size_t> m_head{0};      // 生产者写: 下一处要写入的位置
    std::atomic<size_t> m_tail{0};      // 消费者写(Seek 也从外面写)
};
