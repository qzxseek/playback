/* @Created On : 2026/9/15
   @Author : 孟源
   @note : 波形峰值计算实现(平台无关)
*/
#include "audio_sdk/waveform.h"
#include "audio_sdk/encrypted_format.h"

#include <cstring>      
#include <filesystem>  
#include <fstream>
#include <vector>

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

/**
 * @brief 扫描文件里的整段 PCM, 算出整个文件的波形(降采样成 points 个峰值点)
 * @param utf8Path      文件路径(UTF-8)
 * @param payloadOffset data 块载荷在文件里的起始偏移
 * @param dataSize      data 块载荷长度
 * @param bEncrypted    密文容器: 边读边按载荷内偏移解密
 * @param out           输出 [min0,max0,min1,max1,...], 共 2*points 个 float
 * @param points        峰值点个数
 * @return 文件打开并扫完返回 true; 打不开返回 false(out 全部置 0)
 */
bool CWaveform::BuildFilePeaks(const char* utf8Path, size_t payloadOffset, size_t dataSize,
                               bool bEncrypted, float* out, int points)
{
    if (!utf8Path || !out || points <= 0) return false;

    const size_t total = static_cast<size_t>(points) * 2;
    for (size_t i = 0; i < total; ++i) out[i] = 0.0f;

    std::ifstream f(std::filesystem::u8path(utf8Path), std::ios::in | std::ios::binary);
    if (!f.is_open()) return false;

    // 每个点覆盖的采样数与"整份算"完全一致(sampleCount / points, 至少 1),
    // 这样分块扫描出来的结果和以前整份算的逐个点都对得上
    size_t samples = dataSize / sizeof(int16_t) / static_cast<size_t>(points);
    if (samples == 0) samples = 1;
    const size_t stepBytes = samples * sizeof(int16_t);

    f.seekg(static_cast<std::streamoff>(payloadOffset));

    // 固定 64KB 读缓冲, 一个点可能覆盖好几 MB. 所以点内再分几次读、把峰值并起来
    std::vector<uint8_t> buf(64 * 1024);
    size_t readPos = 0;                    // 载荷内偏移: 加密时拿它定密钥相位
    bool   ok      = true;

    for (int i = 0; i < points && ok; ++i) {
        const size_t begin = static_cast<size_t>(i) * stepBytes;
        // 最后一个点吃掉剩余全部, 否则 dataSize 不是 points 整数倍时尾部那段数据永远画不出来
        size_t end = (i == points - 1) ? dataSize : begin + stepBytes;
        if (end > dataSize) end = dataSize;

        if (begin >= dataSize) {           // 点数比数据还多: 补 0,
            continue;                      // 上面已经整体置 0 了
        }

        float mn = 0.0f, mx = 0.0f;
        bool  any = false;
        while (readPos < end) {
            size_t want = (std::min)(buf.size(), end - readPos);
            if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want))) {
                ok = false;                // 文件被截断: 剩下的点保持 0, 不再往下读
                break;
            }
            if (bEncrypted)
                CEncryptedFormat::XorCrypt(buf.data(), want, readPos);

            float mm[2] = {};
            ComputePeaks(buf.data(), want, mm, 1);   // 这一段整个属于同一个点
            if (!any || mm[0] < mn) mn = mm[0];
            if (!any || mm[1] > mx) mx = mm[1];
            any = true;
            readPos += want;
        }
        if (any) {
            out[i * 2]     = mn;
            out[i * 2 + 1] = mx;
        }
    }
    return ok;
}
