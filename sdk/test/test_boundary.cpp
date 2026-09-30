/* @Created On : 2026/9/29
   @Author : 孟源
   @note : 边界测试 —— 合法但处于两头极限的输入: 0 帧、1 帧、非标准参数、
          奇数子块填充、环绕满一圈、Seek 到末尾之外。
          这些输入"不该报错", 但也最容易在下标/取模上写错一位。

          大部分不需要设备; 只有 Seek 那条要真的开着播放器。
*/
#include "test_common.h"

#include "audio_sdk/audio_player.h"
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/wave_ring.h"
#include "audio_sdk/waveform.h"
#include "audio_sdk/wav_validate.h"

#include <windows.h>   // Sleep, GetTickCount

// ===========================================================================
// 数据量的两头
// ===========================================================================

/**
 * @brief data 长度为 0: 合法但要能善终 —— 校验通过, 长度和偏移都对
 * @note 录音刚起、还没攒够一块就可能出现这种文件
 */
static void TestZeroLengthData() {
    std::vector<uint8_t> buf = tst::MakeWav(0);
    CHECK(buf.size() == sizeof(WavHeader), "0 帧的文件就是 44 字节");

    WavValidate v;
    CHECK(v.Validate(buf.data(), buf.size()), "data 长度 0 的 WAV 校验通过");
    CHECK(v.GetDataSize() == 0, "解出的 data 长度为 0");
    CHECK(v.GetDataOffset() == sizeof(WavHeader), "data 偏移仍指向 44");
}

/**
 * @brief 恰好 1 帧数据: 不能被当成"空"或"残帧"误判
 * @note blockAlign 是这里唯一的除数, 1 帧正好是它能整除的最小单位
 */
static void TestExactlyOneFrame() {
    const uint32_t frameBytes = CHANNELS * (BITS_PER_SAMPLE / 8);   // 2
    std::vector<uint8_t> buf = tst::MakeWav(1);
    CHECK(buf.size() == sizeof(WavHeader) + frameBytes, "1 帧文件大小正确");

    WavValidate v;
    CHECK(v.Validate(buf.data(), buf.size()), "1 帧的 WAV 校验通过");
    CHECK(v.GetDataSize() == frameBytes, "data 长度 = 1 帧");
}

// ===========================================================================
// 格式参数
// ===========================================================================

/**
 * @brief 非标准但自洽的参数组合: 低采样率 / 8 位 / 双声道 / 32 位都该通过
 * @note 校验器只要求"自洽", 不限定采样率与位深的具体值 —— 别把合法值误杀
 */
static void TestNonStandardFormats() {
    struct Case { const char* name; uint32_t rate; uint16_t ch; uint16_t bits; };
    const Case cases[] = {
        { "8000Hz  16bit 单声道",  8000,  1, 16 },
        { "44100Hz 8bit  单声道", 44100,  1,  8 },
        { "44100Hz 16bit 双声道", 44100,  2, 16 },
        { "48000Hz 32bit 单声道", 48000,  1, 32 },
        { "96000Hz 32bit 双声道", 96000,  2, 32 },
    };

    for (const Case& c : cases) {
        const std::vector<uint8_t> buf = tst::MakeWav(256, c.rate, c.ch, c.bits);
        WavValidate v;
        const bool ok = v.Validate(buf.data(), buf.size());
        CHECK_D(ok, c.name, "这是自洽的头, 不该被拒");
        if (!ok) continue;
        CHECK(v.Header().sampleRate == c.rate, "采样率读回一致");
        CHECK(v.Header().numChannels == c.ch, "声道数读回一致");
        CHECK(v.Header().bitsPerSample == c.bits, "位深读回一致");
        CHECK(v.GetDataSize() == 256u * c.ch * (c.bits / 8), "data 长度按参数算对");
    }
}

/**
 * @brief 奇数长度子块的 1 字节填充: 跳块时不补这一字节, 后面全错位
 * @note WAV 规定块负载为奇数时补 1 字节对齐 —— 这是最经典的差一位
 */
static void TestOddChunkPadding() {
    const std::vector<uint8_t> base = tst::MakeWav(512);
    const size_t pcmBytes = base.size() - sizeof(WavHeader);
    const uint8_t* pcm = base.data() + sizeof(WavHeader);

    std::vector<uint8_t> buf(base.begin(), base.begin() + 12);   // RIFF 头
    const char junk[8] = { 'J', 'U', 'N', 'K', 3, 0, 0, 0 };     // 负载 3 字节(奇数)
    buf.insert(buf.end(), junk, junk + 8);
    buf.insert(buf.end(), 3, 0);                                 // 负载
    buf.insert(buf.end(), 1, 0);                                 // 填充字节
    buf.insert(buf.end(), base.begin() + 12, base.end());
    tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8));

    // 排在前面的只有 RIFF(12) 与 fmt 段(24); 垫在 fmt 前的块整块往 44 之后推
    WavValidate v;
    CHECK(v.Validate(buf.data(), buf.size()), "JUNK 负载为奇数时仍校验通过");
    CHECK(v.GetDataOffset() == sizeof(WavHeader) + 12,
          "data 偏移跳过了负载 + 1 字节填充");
    CHECK(std::memcmp(buf.data() + v.GetDataOffset(), pcm, pcmBytes) == 0,
          "取到的 data 与原始 PCM 一致");
}

/**
 * @brief 200 个额外子块连着堆在 fmt 前: 跳块循环要能一路走过去
 * @note 单块跳对了不代表循环对了; 这里顺带验一下"块多也不会走飞"
 */
static void TestManyChunks() {
    const std::vector<uint8_t> base = tst::MakeWav(512);
    const size_t pcmBytes = base.size() - sizeof(WavHeader);
    const uint8_t* pcm = base.data() + sizeof(WavHeader);

    std::vector<uint8_t> buf(base.begin(), base.begin() + 12);
    size_t extra = 0;
    for (int i = 0; i < 200; ++i) {
        const char id[8] = { 'J', 'U', 'N', 'K', 2, 0, 0, 0 };
        buf.insert(buf.end(), id, id + 8);
        buf.insert(buf.end(), 2, 0);
        extra += 10;
    }
    buf.insert(buf.end(), base.begin() + 12, base.end());
    tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8));

    WavValidate v;
    CHECK(v.Validate(buf.data(), buf.size()), "200 个前置子块仍校验通过");
    CHECK(v.GetDataOffset() == sizeof(WavHeader) + extra, "data 偏移算对");
    CHECK(std::memcmp(buf.data() + v.GetDataOffset(), pcm, pcmBytes) == 0,
          "取到的 data 与原始 PCM 一致");
}

// ===========================================================================
// 波形环
// ===========================================================================

/**
 * @brief 环正好绕满一圈: 读到的点数、顺序都要对
 * @note 这正是 CWaveRing 用"单调递增游标"而不是"取模游标"的意义 ——
 *       老的 (w - r + N) % N 在写方正好绕满一圈时算出 0, 环满却被判成"没数据"
 */
static void TestWaveRingWrap() {
    CWaveRing ring;
    const int cap = AUDIO_SDK_WAVE_RING_POINTS;
    const int blk = AUDIO_SDK_WAVE_BLOCK_POINTS;      // 256
    const int nblk = cap / blk;                       // 16 块正好写满

    std::vector<float> in(blk * 2), out(blk * 2);

    // 写满正好一圈; 用块号当标记, 好验顺序
    for (int b = 0; b < nblk; ++b) {
        for (int i = 0; i < blk; ++i) {
            in[i * 2]     = static_cast<float>(b);
            in[i * 2 + 1] = static_cast<float>(b);
        }
        ring.Push(in.data(), blk);
    }

    int expect = 0, total = 0, got = 0;
    bool orderOk = true, sizeOk = true;
    for (;;) {
        const int n = ring.Pop(out.data(), blk);
        if (n <= 0) break;
        if (n != blk) sizeOk = false;
        total += n;
        ++got;
        if (out[0] != static_cast<float>(expect)) orderOk = false;
        ++expect;
    }

    CHECK(total == cap, "绕满一圈后取回的点数 = 环容量(没被误判成空)");
    CHECK(got == nblk, "取回的批次数与写入块数一致");
    CHECK(sizeOk, "每批都是满块");
    CHECK(orderOk, "顺序是 最老 → 最新");

    // 游标已经回绕到 0 附近, 再写一块仍要取得对
    for (int i = 0; i < blk; ++i) { in[i * 2] = 99.0f; in[i * 2 + 1] = 99.0f; }
    ring.Push(in.data(), blk);
    CHECK(ring.Pop(out.data(), blk) == blk, "回绕后再写一块, 取到完整一块");
    CHECK(out[0] == 99.0f, "取到的正是刚写进去的数据");
    CHECK(ring.Pop(out.data(), blk) == 0, "取空后返回 0");
}

/**
 * @brief 写方远超一整圈(调用方太久没取): 只保留最新的满满一环, 丢最老的
 * @note 这是约定的取舍 —— 丢的是波形显示, 不动录音数据
 */
static void TestWaveRingOverrun() {
    CWaveRing ring;
    const int cap = AUDIO_SDK_WAVE_RING_POINTS;
    const int blk = AUDIO_SDK_WAVE_BLOCK_POINTS;
    const int nblk = cap / blk;

    std::vector<float> in(blk * 2), out(blk * 2);

    // 写两圈半, 一直不取
    const int extra = nblk / 2;
    for (int b = 0; b < nblk * 2 + extra; ++b) {
        for (int i = 0; i < blk; ++i) {
            in[i * 2] = in[i * 2 + 1] = static_cast<float>(b);
        }
        ring.Push(in.data(), blk);
    }

    // 期望: 拿到的第一块是最新 cap 点的起点, 也就是块号 (总块数 - nblk)
    const int firstExpected = nblk * 2 + extra - nblk;

    int total = 0, first = -1;
    for (;;) {
        const int n = ring.Pop(out.data(), blk);
        if (n <= 0) break;
        if (first < 0) first = static_cast<int>(out[0]);
        total += n;
    }
    std::printf("         总块 %d, 取回 %d 点, 首块号 %d(期望 %d)\n",
                nblk * 2 + extra, total, first, firstExpected);

    CHECK(total == cap, "超圈后取回的点数被截到环容量");
    CHECK(first == firstExpected, "丢掉的是最老的, 保留最新的满满一环");
}

/**
 * @brief 环的入参边界: 空指针 / 0 / 负数 / 单次超容量推入, 都不能越界
 */
static void TestWaveRingArgs() {
    CWaveRing ring;
    const int cap = AUDIO_SDK_WAVE_RING_POINTS;
    std::vector<float> out(8);

    CHECK(ring.Pop(out.data(), 0) == 0, "空环 Pop 返回 0");
    CHECK(ring.Pop(nullptr, 8) == 0, "Pop 空指针返回 0");
    CHECK(ring.Pop(out.data(), -1) == 0, "Pop 负点数返回 0");

    ring.Push(nullptr, 8);                       // 不该崩
    CHECK(ring.Pop(out.data(), 8) == 0, "Push 空指针后环仍为空");

    ring.Push(out.data(), 0);
    CHECK(ring.Pop(out.data(), 8) == 0, "Push 0 点后环仍为空");

    // 单次推入超过整个环: 只留最后 cap 个
    std::vector<float> big((cap + 512) * 2);
    for (int i = 0; i < cap + 512; ++i) {
        big[i * 2] = big[i * 2 + 1] = static_cast<float>(i);
    }
    ring.Push(big.data(), cap + 512);

    std::vector<float> got(cap * 2);
    const int n = ring.Pop(got.data(), cap * 2);   // maxPoints 给得比容量大
    CHECK(n == cap, "超容量推入后只剩环容量那么多点");
    CHECK(got[0] == 512.0f, "保留的是最后 cap 个(前 512 个被丢)");

    ring.Reset();
    CHECK(ring.Pop(out.data(), 8) == 0, "Reset 后环为空");
}

// ===========================================================================
// 波形峰值计算
// ===========================================================================

/**
 * @brief ComputePeaks 的两头: 单点吃全局、点数多于采样时补 0、空入参不做事
 * @note 归一化按 32768 除: -32768 → -1.0, 32767 → 0.99997, 都不该越出 [-1,1]
 */
static void TestComputePeaksBounds() {
    // ---- 单点吃全局: 采样在 ±满量程之间摆 ----
    {
        const int16_t pcm[] = { 32767, -32768, 32767, -32768 };
        float out[2] = { 0, 0 };
        CWaveform::ComputePeaks(pcm, sizeof(pcm), out, 1);
        CHECK(out[0] == -1.0f, "最小峰值 = -1.0(满量程负)");
        CHECK(out[1] > 0.999f && out[1] < 1.0f, "最大峰值 ≈ 0.99997(满量程正)");
        CHECK(out[0] >= -1.0f && out[1] <= 1.0f, "归一化结果不越出 [-1,1]");
    }

    // ---- 点数多于采样数: 多出来的点补 0 ----
    {
        const int16_t pcm[] = { 1000, -1000, 2000, -2000 };   // 4 个采样
        std::vector<float> out(16 * 2, 7.0f);                  // 预填 7 便于查"有没有写"
        CWaveform::ComputePeaks(pcm, sizeof(pcm), out.data(), 16);
        CHECK(out[0] == 1000.0f / 32768.0f, "第 1 点是第 1 个采样");
        CHECK(out[3 * 2] == -2000.0f / 32768.0f, "第 4 个点用了第 4 个采样");
        CHECK(out[4 * 2] == 0.0f && out[4 * 2 + 1] == 0.0f,
              "采样用完后补 0, 不是留着旧值");
        CHECK(out[15 * 2] == 0.0f && out[15 * 2 + 1] == 0.0f, "最后一个点也是 0");
    }

    // ---- 点数少于采样数: 最后一个点吃掉剩余全部(尾部数据不能丢) ----
    {
        int16_t pcm[100];
        for (int i = 0; i < 100; ++i) pcm[i] = 0;
        pcm[99] = 30000;                                       // 只在最后一颗上埋峰值
        float out[2 * 3] = { 0, 0, 0, 0, 0, 0 };
        CWaveform::ComputePeaks(pcm, sizeof(pcm), out, 3);
        CHECK(out[2 * 2 + 1] == 30000.0f / 32768.0f,
              "最后一个点吃到了末尾的采样(尾部峰值没丢)");
    }

    // ---- 空入参: 一个字节都不许写 ----
    {
        float out[2] = { 7.0f, 7.0f };
        CWaveform::ComputePeaks(nullptr, 0, out, 1);
        CHECK(out[0] == 7.0f && out[1] == 7.0f, "空数据源: 输出原地不动");

        const int16_t pcm[] = { 1, 2 };
        CWaveform::ComputePeaks(pcm, sizeof(pcm), nullptr, 1);        // 不该崩
        CWaveform::ComputePeaks(pcm, sizeof(pcm), out, 0);
        CHECK(out[0] == 7.0f, "points=0: 输出原地不动");

        CWaveform::ComputePeaks(pcm, 1, out, 1);                       // 不足一个采样
        CHECK(out[0] == 7.0f, "字节数不足一个 int16: 输出原地不动");
    }
}

// ===========================================================================
// 取波形 / Seek(需要设备)
// ===========================================================================

/**
 * @brief ReadWave 的入参边界: 0 / 负数 / 空指针, 都不能写坏缓冲
 * @note 不用开录音也能验 —— 环为空时这些调用本来就该原样返回 0
 */
static void TestReadWaveBounds() {
    CAudioRecorder rec;                       // 未开始录音, 环是空的
    float quiet[2] = { -9.0f, -9.0f };

    CHECK(rec.ReadWave(quiet, 1) == 0, "未录音时 ReadWave 返回 0");
    CHECK(rec.ReadWave(quiet, 0) == 0, "maxPoints=0 返回 0");
    CHECK(rec.ReadWave(quiet, -5) == 0, "maxPoints 为负返回 0");
    CHECK(rec.ReadWave(nullptr, 16) == 0, "outMinMax 为 NULL 返回 0");
    CHECK(quiet[0] == -9.0f && quiet[1] == -9.0f, "缓冲没被写过");
}

/**
 * @brief Seek 的两头: 0 / 远超总长 / 停止后 —— 位置必须夹在 [0, 总长] 内
 * @note 需要声卡。位置由播放线程推进, 所以只断言"不越界", 不断言精确值。
 *
 *       注意顺序: Seek 到末尾会让文件当场播完、播放线程退出, 之后的 Seek 会被
 *       当成"没在播"而拒掉。所以"超长夹住"这条必须在还在播的时候测。
 */
static void TestSeekClamp() {
    const std::string path = "t_seek_bound.wav";
    const std::vector<uint8_t> wav = tst::MakeWav(SAMPLE_RATE * 2);   // 2 秒, 够长
    if (!tst::WriteAll(path, wav)) { CHECK(false, "造测试文件失败"); return; }

    CAudioPlayer player;
    const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
    if (tst::DeviceUnavailable(st)) {
        tst::Skip("Seek 边界", "没有可用播放设备");
        std::remove(path.c_str());
        return;
    }
    if (st != AudioSdk::AudioSdkState::NONE) { CHECK(false, "起播失败"); std::remove(path.c_str()); return; }

    const uint32_t total = player.GetTotalPos();
    CHECK(total > 0, "总字节数 > 0");

    // 回到开头: 最普通的一次定位
    CHECK(player.Seek(0) == AudioSdk::AudioSdkState::NONE, "Seek(0) 成功");
    ::Sleep(80);
    CHECK(player.GetPlayPos() <= total, "Seek 到 0 后位置不越界");

    // 远超总长: 应当夹到末尾再记账 —— 不夹的话, 播放线程按这个位置取数据就越界了
    CHECK(player.Seek(0xFFFFFFFFu) == AudioSdk::AudioSdkState::NONE,
          "Seek(远超总长) 不报错, 而是夹住");
    CHECK(player.GetPlayPos() <= total, "超出总长后位置仍被夹在总长内");

    player.StopPlay();

    // 停播后再 Seek: 应当被拒(没有正在播的东西可定位)
    CHECK(player.Seek(0) == AudioSdk::AudioSdkState::INVALID_PARAMETER,
          "未播放时 Seek → INVALID_PARAMETER");

    std::remove(path.c_str());
}

int main() {
    tst::Begin("边界测试");
    RUN(TestZeroLengthData);
    RUN(TestExactlyOneFrame);
    RUN(TestNonStandardFormats);
    RUN(TestOddChunkPadding);
    RUN(TestManyChunks);
    RUN(TestWaveRingWrap);
    RUN(TestWaveRingOverrun);
    RUN(TestWaveRingArgs);
    RUN(TestComputePeaksBounds);
    RUN(TestReadWaveBounds);
    RUN(TestSeekClamp);
    return tst::Summary("边界");
}
