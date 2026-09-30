/* @Created On : 2026/9/29
   @Author : 孟源
   @note : 正向测试 —— 合法输入必须走通, 且产出对得上。

          分两段:
            前半(含 ExtraChunkOffsets)是格式层, 不需要任何设备, 任何机器上都跑;
            后半是录音/播放/闭环, 需要麦克风与声卡 —— 设备打不开就 [skip]。
*/
#include "../include/test_common.h"

#include "audio_sdk/audio_player.h"
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/wav_validate.h"

#include <windows.h>   // Sleep

// ===========================================================================
// 格式层
// ===========================================================================

/**
 * @brief 明文 WAV 存取往返: 存盘 → 读回 → 校验通过 → 头字段与 PCM 逐字节都对
 * @note 不需要设备。这是最基础的一条链路, 它坏了后面全都无从谈起
 */
static void TestWavRoundTrip() {
    const std::string path = "t_wav_roundtrip.wav";
    const std::vector<uint8_t> src = tst::MakeWav(SAMPLE_RATE);   // 1 秒
    const size_t pcmBytes = src.size() - sizeof(WavHeader);

    const AudioSdk::AudioSdkState st =
        CWavFormat::SaveWavFile(path.c_str(), src.data() + sizeof(WavHeader),
                                pcmBytes, /*bEncrypt=*/false);
    CHECK(st == AudioSdk::AudioSdkState::NONE, "SaveWavFile(明文) 返回 NONE");

    const std::vector<uint8_t> back = tst::ReadAll(path);
    CHECK(!back.empty(), "落盘文件存在且非空");
    if (back.empty()) return;      // 空缓冲再往下走就是 nullptr + 偏移的越界读
    CHECK(back.size() == sizeof(WavHeader) + pcmBytes, "落盘大小 = 44 + PCM 长度");

    WavValidate v;
    CHECK(v.Validate(back.data(), back.size()), "写出的文件能通过校验");
    CHECK(v.Header().sampleRate == SAMPLE_RATE, "采样率读回一致");
    CHECK(v.Header().numChannels == CHANNELS, "声道数读回一致");
    CHECK(v.Header().bitsPerSample == BITS_PER_SAMPLE, "位深读回一致");
    CHECK(v.GetDataSize() == pcmBytes, "data 长度读回一致");
    CHECK(v.GetDataOffset() == sizeof(WavHeader), "标准排布下 data 偏移 = 44");
    CHECK(std::memcmp(back.data() + v.GetDataOffset(),
                      src.data() + sizeof(WavHeader), pcmBytes) == 0,
          "PCM 逐字节一致(没有被重新编码/截断)");

    std::remove(path.c_str());
}

/**
 * @brief 加密 .aenc 存取往返: 前缀可识别 → 剥 6 字节后仍是合法 WAV → XOR 解回明文一致
 * @note 不需要设备。重点验"加密只是外面套一层壳", 壳里面的结构与明文完全相同
 */
static void TestAencRoundTrip() {
    const std::string path = "t_aenc_roundtrip.aenc";
    const std::vector<uint8_t> src = tst::MakeWav(SAMPLE_RATE / 2);   // 0.5 秒
    const uint8_t* pcm = src.data() + sizeof(WavHeader);
    const size_t pcmBytes = src.size() - sizeof(WavHeader);

    CHECK(CWavFormat::SaveWavFile(path.c_str(), pcm, pcmBytes, /*bEncrypt=*/true)
              == AudioSdk::AudioSdkState::NONE,
          "SaveWavFile(加密) 返回 NONE");

    const std::vector<uint8_t> back = tst::ReadAll(path);
    CHECK(back.size() > CEncryptedFormat::kAencPrefixSize, "落盘文件存在且非空");
    if (back.size() <= CEncryptedFormat::kAencPrefixSize) return;   // 否则长度会下溢成超大值
    CHECK(back.size() == CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader) + pcmBytes,
          "落盘大小 = 6(前缀) + 44(头) + PCM");
    CHECK(CEncryptedFormat::IsAencData(back.data(), back.size()), "前 4 字节是 AENC 魔数");
    CHECK(CEncryptedFormat::IsAencFile(path.c_str()), "IsAencFile 判为加密容器");

    // 剥掉前缀后, 内部就是一个标准 WAV
    WavValidate v;
    CHECK(v.Validate(back.data() + CEncryptedFormat::kAencPrefixSize,
                     back.size() - CEncryptedFormat::kAencPrefixSize),
          "剥掉 6 字节前缀后能通过 WAV 校验");
    CHECK(v.GetDataSize() == pcmBytes, "内部 data 长度与原始 PCM 一致");

    // 密文必须真的变了, 且 XOR 一趟能还原
    const uint8_t* cipher = back.data() + CEncryptedFormat::kAencPrefixSize + v.GetDataOffset();
    CHECK(std::memcmp(cipher, pcm, pcmBytes) != 0, "落盘的数据段确实是密文(与明文不同)");

    std::vector<uint8_t> plain(cipher, cipher + pcmBytes);
    CEncryptedFormat::XorCrypt(plain.data(), plain.size());
    CHECK(std::memcmp(plain.data(), pcm, pcmBytes) == 0, "XOR 解回后与原始 PCM 一致");

    std::remove(path.c_str());
}

/**
 * @brief 带额外子块的 WAV: fmt 前垫 JUNK、data 后跟 LIST, 都要能定位到真实 data
 * @note 不需要设备。WAV 规范只保证 fmt 在 data 之前, 别处可以插块,
 *       真实世界的录音软件常这么干 —— 按"44 字节后就是数据"读会读错
 */
static void TestExtraChunkOffsets() {
    const std::vector<uint8_t> base = tst::MakeWav(1024);
    const size_t pcmBytes = base.size() - sizeof(WavHeader);
    const uint8_t* pcm = base.data() + sizeof(WavHeader);

    // ---- 情形一: fmt 前插一个 JUNK 块(负载 4 字节, 偶数不补位) ----
    {
        std::vector<uint8_t> buf(base.begin(), base.begin() + 12);   // RIFF 头 12 字节
        const char junk[8] = {'J', 'U', 'N', 'K', 4, 0, 0, 0};
        buf.insert(buf.end(), junk, junk + 8);
        buf.insert(buf.end(), 4, 0);                                 // 负载
        buf.insert(buf.end(), base.begin() + 12, base.end());        // 原 fmt + data
        tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8));

        // 注意: 标准头里排在 data 载荷前面的只有 RIFF(12) 和 fmt 段(24),
        // 所以垫在 fmt 前的额外块是"整块"地往 44 之后再推
        const size_t expectOff = sizeof(WavHeader) + 12;
        WavValidate v;
        CHECK(v.Validate(buf.data(), buf.size()), "JUNK 前置的文件校验通过");
        CHECK(v.GetDataSize() == pcmBytes, "data 长度解析正确");
        CHECK(v.GetDataOffset() == expectOff, "data 偏移跳过了 JUNK 块");
        CHECK(std::memcmp(buf.data() + v.GetDataOffset(), pcm, pcmBytes) == 0,
              "取到的 data 与原始 PCM 一致");
    }

    // ---- 情形二: data 后追加一个 LIST 尾块 ----
    {
        std::vector<uint8_t> buf = base;
        const char list[8] = {'L', 'I', 'S', 'T', 6, 0, 0, 0};
        buf.insert(buf.end(), list, list + 8);
        buf.insert(buf.end(), 6, 0);
        tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8));

        WavValidate v;
        CHECK(v.Validate(buf.data(), buf.size()), "data 后带 LIST 的文件校验通过");
        CHECK(v.GetDataSize() == pcmBytes,
              "data 长度按块内声明 —— 不把尾块算进数据里");
        CHECK(v.GetDataOffset() == sizeof(WavHeader), "data 偏移仍是 44");
    }
}

// ===========================================================================
// 录音 / 播放(需要设备)
// ===========================================================================

/**
 * @brief 录音主管线: 录 N 秒 → 停止落盘 → 文件是 .aenc 且内部时长 ≈ N 秒
 * @note 需要麦克风。同时验"计时器"和"真实数据量"两条 —— 光看计时器不知道有没有采到声
 */
static void TestRecordPipeline() {
    const std::string base = "t_record_pipeline";
    const DWORD seconds = 3;

    CAudioRecorder rec;
    rec.SetOutputPath(base.c_str());
    tst::SetEncrypt(rec, true);
    const std::string file = tst::RecFileName(base, rec);

    const AudioSdk::AudioSdkState st = rec.StartRecording();
    if (tst::DeviceUnavailable(st)) { tst::Skip("录音主管线", "没有可用录音设备"); return; }
    CHECK(st == AudioSdk::AudioSdkState::NONE, "StartRecording 返回 NONE");
    if (st != AudioSdk::AudioSdkState::NONE) return;

    ::Sleep(seconds * 1000);

    const uint32_t recordedMs = rec.GetRecordedMs();
    std::printf("         计时器报 %u ms\n", recordedMs);
    CHECK(recordedMs > (seconds - 1) * 1000 && recordedMs < (seconds + 1) * 1000,
          "已录时长 ≈ 实际经过时间");

    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "StopRecording 返回 NONE");

    const std::vector<uint8_t> buf = tst::ReadAll(file);
    CHECK(!buf.empty(), "落盘文件存在且非空");
    if (buf.empty()) return;

    CHECK(CEncryptedFormat::IsAencData(buf.data(), buf.size()), "落盘的是 .aenc 容器");

    WavValidate v;
    CHECK(v.Validate(buf.data() + CEncryptedFormat::kAencPrefixSize,
                     buf.size() - CEncryptedFormat::kAencPrefixSize),
          "内部 WAV 头合法");

    const size_t bytesPerSec = static_cast<size_t>(SAMPLE_RATE) * CHANNELS * (BITS_PER_SAMPLE / 8);
    const size_t gotMs = v.GetDataSize() * 1000 / bytesPerSec;
    std::printf("         落盘 data %zu 字节 ≈ %zu ms\n", v.GetDataSize(), gotMs);

    CHECK(gotMs > (seconds - 1) * 1000 && gotMs < (seconds + 1) * 1000,
          "落盘数据时长 ≈ 录音秒数");
    CHECK(v.GetDataSize() >= bytesPerSec * AUDIO_SDK_BLOCK_MS / 1000,
          "至少录到一个完整块(一个块都不到说明根本没采到声)");

    // 环里的波形也应当有东西: 1 秒 ≈ 10 块 × 256 点
    float pull[AUDIO_SDK_WAVE_BLOCK_POINTS * 2];
    int pts = 0, batches = 0;
    for (;;) {
        const int n = rec.ReadWave(pull, AUDIO_SDK_WAVE_BLOCK_POINTS);
        if (n <= 0) break;
        pts += n;
        if (++batches > 1000) break;      // 防死循环: 恒返回正数的话这里会挂死
    }
    std::printf("         波形点 %d\n", pts);
    CHECK(pts > 0, "录音过程中产生了波形点");

    std::remove(file.c_str());
}

/**
 * @brief 播放明文 WAV: 总长对得上 → 自然播完 → 位置推进到末尾
 * @note 需要声卡
 */
static void TestPlayWav() {
    const std::string path = "t_play_wav.wav";
    const std::vector<uint8_t> wav = tst::MakeWav(SAMPLE_RATE / 2);   // 0.5 秒
    if (!tst::WriteAll(path, wav)) { CHECK(false, "造测试文件失败"); return; }
    const uint32_t pcmBytes = static_cast<uint32_t>(wav.size() - sizeof(WavHeader));

    CAudioPlayer player;
    const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
    if (tst::DeviceUnavailable(st)) {
        tst::Skip("播放明文 WAV", "没有可用播放设备");
        std::remove(path.c_str());
        return;
    }
    CHECK(st == AudioSdk::AudioSdkState::NONE, "PlayWavFile 返回 NONE");
    if (st != AudioSdk::AudioSdkState::NONE) { std::remove(path.c_str()); return; }

    CHECK(player.GetTotalPos() == pcmBytes, "总字节数 = PCM 长度");
    const uint32_t totalMs = player.GetTotalPosMs();
    std::printf("         总时长 %u ms\n", totalMs);
    CHECK(totalMs > 400 && totalMs < 600, "总时长 ≈ 500ms");
    CHECK(player.IsPlaying(), "IsPlaying 为真");
    CHECK(!player.IsPaused(), "刚起播不是暂停态");

    const DWORD t0 = ::GetTickCount();
    while (player.IsPlaying() && ::GetTickCount() - t0 < 1500) ::Sleep(20);   // 差值式, 回绕安全

    CHECK(!player.IsPlaying(), "播完后 IsPlaying 转假");
    CHECK(player.GetPlayPos() >= pcmBytes * 9 / 10, "播放位置推进到接近末尾");
    CHECK(player.GetPlayPosMs() > 400, "按毫秒算也接近末尾");

    player.StopPlay();
    std::remove(path.c_str());
}

/**
 * @brief 播放加密 .aenc: 解密成功 → 总时长与明文版一致
 * @note 需要声卡。这是"加密容器能被播放链正确解开"的端到端验证
 */
static void TestPlayAenc() {
    const std::string path = "t_play_aenc.aenc";
    const std::vector<uint8_t> wav = tst::MakeWav(SAMPLE_RATE / 2);
    const size_t pcmBytes = wav.size() - sizeof(WavHeader);

    if (CWavFormat::SaveWavFile(path.c_str(), wav.data() + sizeof(WavHeader),
                                pcmBytes, true) != AudioSdk::AudioSdkState::NONE) {
        CHECK(false, "造加密测试文件失败");
        return;
    }

    CAudioPlayer player;
    const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
    if (tst::DeviceUnavailable(st)) {
        tst::Skip("播放加密 AENC", "没有可用播放设备");
        std::remove(path.c_str());
        return;
    }
    CHECK(st == AudioSdk::AudioSdkState::NONE, "PlayWavFile(.aenc) 返回 NONE");
    if (st != AudioSdk::AudioSdkState::NONE) { std::remove(path.c_str()); return; }

    CHECK(player.GetTotalPos() == pcmBytes, "解密后总字节数 = 原始 PCM 长度");
    const uint32_t ms = player.GetTotalPosMs();
    std::printf("         解密后总时长 %u ms\n", ms);
    CHECK(ms > 400 && ms < 600, "解密后总时长 ≈ 500ms");

    player.StopPlay();
    std::remove(path.c_str());
}

/**
 * @brief 闭环: 录一段 → 立刻用播放链打开录出的文件 → 能播且时长对得上
 * @note 需要麦克风与声卡。这是"录出来的东西真的能播"的最终验证
 */
static void TestRecordThenPlay() {
    const std::string base = "t_loop";

    CAudioRecorder rec;
    rec.SetOutputPath(base.c_str());
    tst::SetEncrypt(rec, true);
    const std::string file = tst::RecFileName(base, rec);

    const AudioSdk::AudioSdkState rst = rec.StartRecording();
    if (tst::DeviceUnavailable(rst)) { tst::Skip("录音→播放闭环", "没有可用录音设备"); return; }
    CHECK(rst == AudioSdk::AudioSdkState::NONE, "开始录音");
    if (rst != AudioSdk::AudioSdkState::NONE) return;

    ::Sleep(2000);
    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "停止录音并落盘");

    CAudioPlayer player;
    const AudioSdk::AudioSdkState pst = player.PlayWavFile(file.c_str());
    if (tst::DeviceUnavailable(pst)) {
        tst::Skip("录音→播放闭环", "没有可用播放设备");
        std::remove(file.c_str());
        return;
    }
    CHECK(pst == AudioSdk::AudioSdkState::NONE, "播放链能打开刚录出的 .aenc");
    if (pst != AudioSdk::AudioSdkState::NONE) { std::remove(file.c_str()); return; }

    const uint32_t ms = player.GetTotalPosMs();
    std::printf("         录出文件时长 %u ms\n", ms);
    CHECK(ms > 1500 && ms < 2500, "播放链报出的时长 ≈ 实际录的 2 秒");

    player.StopPlay();
    std::remove(file.c_str());
}

int main() {
    tst::Begin("正向测试");
    RUN(TestWavRoundTrip);
    RUN(TestAencRoundTrip);
    RUN(TestExtraChunkOffsets);
    RUN(TestRecordPipeline);
    RUN(TestPlayWav);
    RUN(TestPlayAenc);
    RUN(TestRecordThenPlay);
    return tst::Summary("正向");
}
