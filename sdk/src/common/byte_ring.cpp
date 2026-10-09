/* @Created On : 2026/10/8
   @Author : 孟源
   @note : 环形字节缓冲实现(平台无关)
*/
#include "audio_sdk/byte_ring.h"

#include <algorithm>
#include <cstring>

CByteRing::CByteRing(size_t capacityBytes){
    size_t cap = 1;
    while (cap < capacityBytes) cap <<= 1;   // 取模要变成按位与, 容量必须是 2 的幂
    m_buf.resize(cap);
    m_mask = cap - 1;
}
CByteRing::~CByteRing(){
    m_buf.clear();
    m_base.store(0, std::memory_order_relaxed);
    m_tail.store(0, std::memory_order_relaxed);
    m_head.store(0, std::memory_order_release);
}
/**
 * @brief 还能写多少字节
 * @return 空闲字节数; 环刚被 Reset 到读游标前面时返回 0(等生产者重新定位)
 */
size_t CByteRing::Space() const{
    const size_t head = m_head.load(std::memory_order_relaxed);
    const size_t tail = m_tail.load(std::memory_order_acquire);
    if (tail > head) return 0;               // 被 SeekTo 甩到前面了, 减出来会下溢
    const size_t used = head - tail;
    return used >= m_buf.size() ? 0 : m_buf.size() - used;
}

/**
 * @brief 写入一段字节(生产者)
 * @param src 源缓冲
 * @param n 想写入的字节数
 * @return 实际写入的字节数; 环满或环被重置到前面时返回 0
 */
size_t CByteRing::Write(const uint8_t* src, size_t n){
    if (!src || n == 0) return 0;

    const size_t head = m_head.load(std::memory_order_relaxed);
    const size_t tail = m_tail.load(std::memory_order_acquire);
    if (tail > head) return 0;                       // 环被 SeekTo/Reset 甩到前面, 等生产者处理

    const size_t used = head - tail;
    const size_t space = used >= m_buf.size() ? 0 : m_buf.size() - used;
    if (space == 0) return 0;
    if (n > space) n = space;

    // 分两段写: 到缓冲末尾为止, 剩下那截绕回开头
    const size_t off = head & m_mask;               // 写入起点在数组里的下标
    const size_t first = m_buf.size() - off;
    if (n <= first){
        std::memcpy(m_buf.data() + off, src, n);
    } else {
        std::memcpy(m_buf.data() + off, src, first);
        std::memcpy(m_buf.data(), src + first, n - first);
    }
    // release: 保证上面写进缓冲的数据, 对读到这个 head 的消费者可见
    m_head.store(head + n, std::memory_order_release);
    return n;
}

/**
 * @brief 丢弃环内全部数据, 从 pos 重开一轮(生产者)
 * @param pos 新的起点(绝对值)
 */
void CByteRing::Reset(size_t pos){
    m_base.store(pos, std::memory_order_relaxed);
    m_tail.store(pos, std::memory_order_relaxed);
    m_head.store(pos, std::memory_order_release);
}

/**
 * @brief 把绝对位置 pos 起的 n 字节拷进 dst(消费者)
 * @param pos 绝对位置
 * @param dst 目标缓冲
 * @param n 字节数
 */
void CByteRing::ReadAt(size_t pos, uint8_t* dst, size_t n) const{
    if (!dst || n == 0) return;
    if (n > m_buf.size()) n = m_buf.size();

    const size_t off = pos & m_mask;
    const size_t first = m_buf.size() - off;
    if (n <= first){
        std::memcpy(dst, m_buf.data() + off, n);
    } else {
        std::memcpy(dst, m_buf.data() + off, first);
        std::memcpy(dst + first, m_buf.data(), n - first);
    }
}

/**
 * @brief 推进读游标(消费者)
 * @param expect 预期当前值
 * @param next 要推进到的新值
 * @return 推进成功返回 true; 期间被 SeekTo 改过则返回 false
 */
bool CByteRing::AdvanceTail(size_t expect, size_t next){
    return m_tail.compare_exchange_strong(expect, next,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire);
}
