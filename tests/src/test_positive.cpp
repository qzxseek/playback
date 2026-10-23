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
#include "audio_sdk/opus_format.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/wav_validate.h"
#include "audio_sdk/waveform.h"

#include <algorithm>   // std::min
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

/**
 * @brief 流式播放的两块地基: 只给文件前缀也能验头、XOR 能从半路开始解
 * @note 不需要设备。播放不再把整段 PCM 读进内存, 靠的就是这两条:
 *       验头只读前缀(长度得拿文件总长去对, 不能拿前缀长度去对);
 *       解密按块做(密钥相位得跟着数据在数据区里的位置走, 不能每块都从 0 起)。
 *       任何一个不对, 流式播出来的就是乱码或直接被拒。
 */
static void TestStreamPlumbing() {
    const std::vector<uint8_t> wav = tst::MakeWav(SAMPLE_RATE / 4);   // 0.25 秒
    const size_t pcmBytes = wav.size() - sizeof(WavHeader);
    const uint8_t* pcm = wav.data() + sizeof(WavHeader);

    // ---- 一、前缀校验: 只给前 64 字节 + 真实总长, 结论必须和整份校验一样 ----
    WavValidate full;
    CHECK(full.Validate(wav.data(), wav.size()), "整份校验通过(基线)");

    const size_t prefix = 64;        // 44 字节头 + 余量, 足够走到 data 块头
    WavValidate head;
    CHECK(head.Validate(wav.data(), prefix, wav.size()), "只给前缀 + 真实总长也验得通过");
    CHECK(head.GetDataOffset() == full.GetDataOffset(), "前缀校验给的 data 偏移与整份一致");
    CHECK(head.GetDataSize() == full.GetDataSize(), "前缀校验给的 data 长度与整份一致");
    CHECK(head.Header().sampleRate == full.Header().sampleRate,
          "前缀校验读出的格式参数与整份一致");

    // 总长是"拿它去对 riffSize"的, 报错就必须拒 —— 否则前缀校验等于没校验
    CHECK(!head.Validate(wav.data(), prefix, wav.size() - 2), "总长少报 2 字节被拒");
    CHECK(!head.Validate(wav.data(), prefix, wav.size() + 2), "总长多报 2 字节被拒");

    // ---- 二、XOR 偏移: 整份加密后, 从任意位置切开分块解, 拼回来要是原明文 ----
    std::vector<uint8_t> cipher(pcm, pcm + pcmBytes);
    CEncryptedFormat::XorCrypt(cipher.data(), cipher.size());      // 相位从 0 起, 即真实密文

    // 从一个 16 的倍数之外的位置单独解一块, 必须等于整份密文的同一段
    const size_t off = 8, one = 16;
    std::vector<uint8_t> slice(pcm + off, pcm + off + one);
    CEncryptedFormat::XorCrypt(slice.data(), one, off);
    CHECK(std::memcmp(slice.data(), cipher.data() + off, one) == 0,
          "单解 offset=8 起的一块 == 整份密文的同一段");

    // 按播放器的方式分块解密: 块长故意都不是 16 的倍数, 拼回来必须逐字节等于原明文
    std::vector<uint8_t> back = cipher;
    const size_t sizes[] = {40, 7, 129, 1000};
    size_t pos = 0;
    for (size_t i = 0; pos < back.size(); ++i) {
        // 加括号是为了躲开 windows.h 的 min 宏(它会把 std::min 也一并替换掉)
        const size_t n = (std::min)(sizes[i % 4], back.size() - pos);
        CEncryptedFormat::XorCrypt(back.data() + pos, n, pos);
        pos += n;
    }
    CHECK(back == std::vector<uint8_t>(pcm, pcm + pcmBytes),
          "按任意块长分块解密后拼回来 == 原明文");

    // ---- 三、边录边写器: 分块写出来的文件, 必须和整份写的逐字节相同 ----
    // 这是把录音改成流式之后唯一不能变的东西 —— 落盘格式一旦漂了, 老录音全读不回来。
    // 容器布局(明文 [WAV头] / 加密 [AENC][ver][WAV头][XOR])与 SaveAencFile 是同一套。
    for (int enc = 0; enc <= 1; ++enc) {
        const bool bEnc = (enc == 1);
        const char* tag = bEnc ? "加密" : "明文";
        const char* ext = bEnc ? ".aenc" : ".wav";
        const std::string pathBatch  = std::string("t_writer_batch")  + ext;
        const std::string pathStream = std::string("t_writer_stream") + ext;
        const size_t headBytes = (bEnc ? CEncryptedFormat::kAencPrefixSize : 0)
                               + sizeof(WavHeader);

        // 基线: 老路径 —— 整份一次写完
        CHECK(CWavFormat::SaveWavFile(pathBatch.c_str(), pcm, pcmBytes, bEnc)
                  == AudioSdk::AudioSdkState::NONE, "整份写(基线) 成功");

        // 新路径: 边录边写。块长故意都不是 16 的倍数, 挤一挤 XOR 的密钥相位
        {
            CWavStreamWriter w;
            CHECK(w.Open(pathStream.c_str(), bEnc) == AudioSdk::AudioSdkState::NONE,
                  "流式写: Open 成功");

            // 开完文件, 头就该已经在盘上 —— 不能留在流缓冲里。
            // 攒着的话, 从外面看开录之后是个 0 字节的空文件, 像"根本没在记";
            // 用户就是照着这个现象判断"数据还在内存里攒着"的。
            CHECK(tst::ReadAll(pathStream).size() == headBytes,
                  "Open 之后占位头就已经落盘(文件不是 0 字节)");

            size_t pos = 0;
            const size_t sizes[] = {40, 7, 129, 1000};
            bool ok = true;
            for (size_t i = 0; pos < pcmBytes; ++i) {
                const size_t n = (std::min)(sizes[i % 4], pcmBytes - pos);
                // Write 会就地把明文改成密文 —— 别把基线还要用的那份明文喂进去
                std::vector<uint8_t> chunk(pcm + pos, pcm + pos + n);
                if (w.Write(chunk.data(), n) != AudioSdk::AudioSdkState::NONE) { ok = false; break; }
                pos += n;
            }
            CHECK(ok, "流式写: 每一块都写进去了");
            CHECK(w.GetWritten() == pcmBytes, "Written() == 写进去的 PCM 字节数");
            CHECK(w.Finish(), "流式写: Finish 成功");
            CHECK(w.Finish(), "Finish 可重复调用(析构还会再调一次)");
        }

        const std::vector<uint8_t> a = tst::ReadAll(pathBatch);
        const std::vector<uint8_t> b = tst::ReadAll(pathStream);
        std::printf("         %s: 整份 %zu 字节 / 流式 %zu 字节\n", tag, a.size(), b.size());
        CHECK(!b.empty(), "流式写出来的文件非空");
        CHECK(a.size() == b.size(), "流式与整份的文件长度一致");
        CHECK(a == b, "流式与整份逐字节一致(容器布局没变)");

        // 写出来的还得是能读回去的: 校验通过 + 解密后 == 原明文
        if (!b.empty() && b.size() > headBytes) {
            WavValidate v;
            CHECK(v.Validate(b.data() + (bEnc ? CEncryptedFormat::kAencPrefixSize : 0),
                             b.size() - (bEnc ? CEncryptedFormat::kAencPrefixSize : 0)),
                  "流式写出来的文件能通过校验");
            std::vector<uint8_t> plain(b.begin() + headBytes, b.end());
            if (bEnc) CEncryptedFormat::XorCrypt(plain.data(), plain.size());
            CHECK(plain == std::vector<uint8_t>(pcm, pcm + pcmBytes),
                  "流式写出来的密文解回去 == 原明文");
        }

        std::remove(pathBatch.c_str());
        std::remove(pathStream.c_str());
    }

    // ---- 四、增量落盘: Finish 之前盘上就该有东西了 ----
    // 这条验的是"流式"本身 —— 数据是一块块写出去的, 不是攒到最后一次性倒出来。
    // 必须写超过文件流自身缓冲的量才有意义(缓冲那是恒定的一小截, 不随录音时长涨),
    // 所以这里单独用一个较大的负载。
    {
        const std::string path = "t_writer_incremental.wav";
        std::remove(path.c_str());

        std::vector<uint8_t> big(256 * 1024, 0x5A);
        CWavStreamWriter w;
        CHECK(w.Open(path.c_str(), false) == AudioSdk::AudioSdkState::NONE, "增量: Open 成功");

        bool ok = true;
        for (size_t off = 0; off < big.size(); off += 4096) {
            const size_t n = (std::min)(size_t{4096}, big.size() - off);
            if (w.Write(big.data() + off, n) != AudioSdk::AudioSdkState::NONE) { ok = false; break; }
        }
        CHECK(ok, "增量: 256KB 全部写入");

        const std::vector<uint8_t> mid = tst::ReadAll(path);
        std::printf("         Finish 前盘上已有 %zu / %zu 字节\n", mid.size(), w.GetWritten());
        CHECK(mid.size() >= big.size() - 64 * 1024,
              "Finish 之前数据就陆续写到盘上了(只有一小截还在流缓冲里)");

        CHECK(w.Finish(), "增量: Finish 成功");
        const std::vector<uint8_t> done = tst::ReadAll(path);
        CHECK(done.size() == sizeof(WavHeader) + big.size(), "增量: 收尾后长度完整");
        std::remove(path.c_str());
    }
}

// ===========================================================================
// 录音 / 播放(需要设备)
// ===========================================================================

// BuildWaveform 的回调: 把 SDK 给的峰值对抄进调用方自己的缓冲
namespace {
struct WaveSink {
    float data[AUDIO_SDK_WAVE_FILE_POINTS * 2] = {};
    int   count = 0;
};

void OnBuildWave(const float* minmax, int points, void* userData) {
    WaveSink* sink = static_cast<WaveSink*>(userData);
    sink->count = points;
    const int n = points < AUDIO_SDK_WAVE_FILE_POINTS ? points : AUDIO_SDK_WAVE_FILE_POINTS;
    for (int i = 0; i < n * 2; ++i) sink->data[i] = minmax[i];
}
}   // namespace

/**
 * @brief 播放器算出的整文件波形 vs 拿明文 PCM 整份算的 —— 必须逐点一致
 * @param player   已经在播某个文件的播放器
 * @param pcm      那个文件的【明文】PCM(不解密的话就是文件里 data 块那段)
 * @note 分块扫描只要有一处边界、步长或解密相位算错, 这里就对不上。
 *       顺带验了"整份算"的老算法和"流式算"的新算法结果相同 —— 换法是换了, 画出来的波形没变。
 */
static void CheckWaveform(CAudioPlayer& player, const uint8_t* pcm, size_t pcmBytes) {
    WaveSink sink;
    player.BuildWaveform(&OnBuildWave, &sink);
    CHECK(sink.count == AUDIO_SDK_WAVE_FILE_POINTS, "BuildWaveform 给出 kFilePoints 个峰值点");
    if (sink.count != AUDIO_SDK_WAVE_FILE_POINTS) return;

    std::vector<float> ref(AUDIO_SDK_WAVE_FILE_POINTS * 2, 0.0f);
    CWaveform::ComputePeaks(pcm, pcmBytes, ref.data(), AUDIO_SDK_WAVE_FILE_POINTS);

    int bad = 0;
    for (int i = 0; i < AUDIO_SDK_WAVE_FILE_POINTS * 2; ++i)
        if (sink.data[i] != ref[i]) ++bad;
    std::printf("         波形逐点比对: 不一致 %d / %d\n", bad, AUDIO_SDK_WAVE_FILE_POINTS * 2);
    CHECK(bad == 0, "流式算的整文件波形与整份算的逐点一致");
}

/**
 * @brief 边录边写: 录制途中数据就已经在文件里了(内存不随录音时长涨的根据)
 * @note 需要麦克风。老实现是"录完才落盘", 录到一半文件根本不存在 ——
 *       这条验的就是那个区别: 头先占位落盘, PCM 一块块追上去, 停止时才回填长度。
 */
static void TestRecordStreamsToDisk() {
    const std::string base = "t_rec_stream";

    CAudioRecorder rec;
    rec.SetOutputPath(base.c_str());
    tst::SetEncrypt(rec, true);
    const std::string file = tst::RecFileName(base, rec);
    std::remove(file.c_str());

    const AudioSdk::AudioSdkState st = rec.StartRecording();
    if (tst::DeviceUnavailable(st)) { tst::Skip("边录边写", "没有可用录音设备"); return; }
    if (st != AudioSdk::AudioSdkState::NONE) { CHECK(false, "开始录音"); return; }

    ::Sleep(600);                                   // 攒够几块

    // ---- 停止之前: 头已经在盘上(长度占位 0), 数据一块块追在后面 ----
    // 老实现是"录完才写", 此刻文件根本不存在。这条就是这次改动的核心断言。
    const std::vector<uint8_t> mid = tst::ReadAll(file);
    const size_t hdrOff = CEncryptedFormat::kAencPrefixSize;
    CHECK(mid.size() > hdrOff + sizeof(WavHeader),
          "停止之前数据就已经落盘了(不是攒在内存里)");
    CHECK(rec.GetRecordedMs() >= AUDIO_SDK_BLOCK_MS, "计时器在走");

    if (mid.size() > hdrOff + sizeof(WavHeader)) {
        WavHeader midHdr{};
        std::memcpy(&midHdr, mid.data() + hdrOff, sizeof(midHdr));
        CHECK(midHdr.dataSize == 0, "还没停止: 头里的长度仍是占位的 0(Finish 才回填)");
        CHECK(midHdr.sampleRate == SAMPLE_RATE, "占位头里的格式参数已经是真的");
    }

    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "停止落盘");

    // ---- 停止之后: 长度回填, 文件是完整合法的 ----
    const std::vector<uint8_t> done = tst::ReadAll(file);
    CHECK(done.size() >= mid.size(), "停止后文件只增不减");
    if (done.size() > hdrOff + sizeof(WavHeader)) {
        WavHeader finHdr{};
        std::memcpy(&finHdr, done.data() + hdrOff, sizeof(finHdr));
        CHECK(finHdr.dataSize == done.size() - hdrOff - sizeof(WavHeader),
              "停止后头里的长度回填成了真实值");

        WavValidate v;
        CHECK(v.Validate(done.data() + hdrOff, done.size() - hdrOff),
              "停止后是完整的合法 .aenc 容器");
    }

    std::remove(file.c_str());
}

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

    CHECK(rec.GetWriteError() == AudioSdk::AudioSdkState::NONE, "录制中 WriteError 为 NONE");

    ::Sleep(seconds * 1000);

    const uint32_t recordedMs = rec.GetRecordedMs();
    std::printf("         计时器报 %u ms\n", recordedMs);
    CHECK(recordedMs > (seconds - 1) * 1000 && recordedMs < (seconds + 1) * 1000,
          "已录时长 ≈ 实际经过时间");

    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "StopRecording 返回 NONE");
    // 取走即清: 这一轮没出过事, 收尾之后也不能留下痕迹影响下一轮
    CHECK(rec.GetWriteError() == AudioSdk::AudioSdkState::NONE, "收尾后 WriteError 复位");

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
 * @brief Opus 编解码往返: 写 .opus/.aopus → 用 COpusStreamReader 解回来
 * @note 不需要设备。解码链(拆页/解密/重采样回放/跳转/波形)只有这一条自动化检查,
 *       编码器、容器或分页哪一环坏了都会在这里挂
 */
static void TestOpusRoundTrip() {
    const std::vector<uint8_t> src = tst::MakeWav(SAMPLE_RATE);   // 1 秒 440Hz 正弦
    const size_t pcmBytes = src.size() - sizeof(WavHeader);

    for (int enc = 0; enc < 2; ++enc) {
        const bool bEnc = (enc == 1);
        const std::string path = bEnc ? "t_opus_roundtrip.aopus" : "t_opus_roundtrip.opus";
        std::remove(path.c_str());

        {
            COpusStreamWriter w;
            CHECK(w.Open(path.c_str(), bEnc) == AudioSdk::AudioSdkState::NONE, "Opus: Open 成功");
            CHECK(w.Write(const_cast<uint8_t*>(src.data()) + sizeof(WavHeader), pcmBytes)
                      == AudioSdk::AudioSdkState::NONE, "Opus: 写入 PCM");
            CHECK(w.Finish(), "Opus: Finish 成功");
        }

        COpusStreamReader r;
        CHECK(r.Open(path.c_str()) == AudioSdk::AudioSdkState::NONE, "Opus: 解码器 Open 成功");
        CHECK(r.GetChannels() == CHANNELS, "Opus: 声道数一致");
        CHECK(r.GetSampleRate() == 48000, "Opus: 解码采样率 48k");
        CHECK(r.GetBlockAlign() == CHANNELS * (BITS_PER_SAMPLE / 8), "Opus: blockAlign 一致");

        // 44.1k → 48k 重采样后 1 秒是 48000 个采样 = 96000 字节
        const size_t total = r.GetTotalBytes();
        std::printf("         %s: %zu 字节 PCM (%.3f s)\n",
                    bEnc ? "aopus" : "opus", total, total / 96000.0);
        CHECK(total + 4000 >= 96000 && total <= 100000, "Opus: 总时长 ≈ 1 秒");

        // 波形图入口要能整条扫一遍(必须在 Read 到 EOF 之前)
        std::vector<float> peaks(CWaveform::kFilePoints * 2, 0.0f);
        CHECK(r.BuildPeaks(peaks.data(), CWaveform::kFilePoints, nullptr, nullptr)
                  == AudioSdk::AudioSdkState::NONE, "Opus: BuildPeaks 成功");
        bool anyPeak = false;
        for (float v : peaks) if (v != 0.0f) { anyPeak = true; break; }
        CHECK(anyPeak, "Opus: BuildPeaks 填出了非零峰值");

        // 重新开一份, 从头读全
        CHECK(r.Open(path.c_str()) == AudioSdk::AudioSdkState::NONE, "Opus: 重新 Open 成功");
        std::vector<uint8_t> out(total);
        CHECK(r.Read(out.data(), out.size()) == total, "Opus: 从头读完整个文件");
        CHECK(r.Read(out.data(), out.size()) == 0, "Opus: 读到底后返回 0");

        float mm[2] = {};
        CWaveform::ComputePeaks(out.data(), out.size(), mm, 1);
        CHECK(mm[1] > 0.2f && mm[0] < -0.2f, "Opus: 解出来的是真实波形, 不是静音");

        // 跳转: 从中间续读, 内容还得是波形(定位没落错地方)
        CHECK(r.SeekTo(total / 2) == AudioSdk::AudioSdkState::NONE, "Opus: SeekTo 成功");
        std::vector<uint8_t> mid(48000);    // 0.5 秒(单声道 16bit)
        const size_t got = r.Read(mid.data(), mid.size());
        CHECK(got == mid.size(), "Opus: 跳转后能继续读");
        float mmMid[2] = {};
        CWaveform::ComputePeaks(mid.data(), got, mmMid, 1);
        CHECK(got > 0 && mmMid[1] > 0.2f, "Opus: 跳转位置解出来的也是波形");

        // Seek(0) 必须与"刚 Open 完从头读"逐字节一致 —— pre-skip 不能漏
        CHECK(r.SeekTo(0) == AudioSdk::AudioSdkState::NONE, "Opus: SeekTo(0) 成功");
        std::vector<uint8_t> head(out.size());
        const size_t nHead = r.Read(head.data(), head.size());
        CHECK(nHead == out.size(), "Opus: Seek(0) 后能读回整段");
        CHECK(nHead == out.size() && head == out,
              "Opus: Seek(0) 的结果与从头读完全一致(pre-skip 没漏)");

        r.Close();
        std::remove(path.c_str());
    }
}

/**
 * @brief OPUS 落盘: 录 1 秒, 产物必须是合法的 Ogg Opus(.aopus 先解壳)
 * @note 需要麦克风。只看扩展名不够 —— 得看容器里的字节, 才能证明 recorder
 *       真的把 PCM 交给了 COpusStreamWriter, 而不是悄悄退回 WAV。
 */
static void TestRecordOpus() {
    const std::string base = "t_record_opus";
    const std::string file = base + ".aopus";
    std::remove(file.c_str());

    CAudioRecorder rec;
    rec.SetOutputPath(base.c_str());
    rec.SetFormat(AudioSdkFormat::OPUS);
    tst::SetEncrypt(rec, true);
    CHECK(rec.GetFormat() == AudioSdkFormat::OPUS, "SetFormat(OPUS) 生效, 没回退 WAV");

    const AudioSdk::AudioSdkState st = rec.StartRecording();
    if (tst::DeviceUnavailable(st)) { tst::Skip("opus 落盘", "没有可用录音设备"); return; }
    CHECK(st == AudioSdk::AudioSdkState::NONE, "StartRecording 返回 NONE");
    if (st != AudioSdk::AudioSdkState::NONE) return;

    ::Sleep(1000);
    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "StopRecording 返回 NONE");

    std::vector<uint8_t> buf = tst::ReadAll(file);
    CHECK(buf.size() > CEncryptedFormat::kAencPrefixSize, "落盘文件存在且非空");
    if (buf.size() <= CEncryptedFormat::kAencPrefixSize) return;
    CHECK(std::memcmp(buf.data(), CEncryptedFormat::kAopusMagic,
                      CEncryptedFormat::kAopusMagicSize) == 0, "开头是 AOPU 魔数");

    // 解壳: 前缀明文直写, 后面的 XOR 相位从 0 起
    std::vector<uint8_t> p(buf.begin() + CEncryptedFormat::kAencPrefixSize, buf.end());
    CEncryptedFormat::XorCrypt(p.data(), p.size(), 0);
    CHECK(p.size() > 27 && std::memcmp(p.data(), "OggS", 4) == 0, "解壳后是 Ogg 流");
    if (p.size() <= 27) return;
    const size_t hlen = 27 + p[26];
    CHECK(hlen + 8 <= p.size() && std::memcmp(p.data() + hlen, "OpusHead", 8) == 0,
          "第一页第一个包是 OpusHead");

    // 逐页走一遍, 拿最后一个非 -1 的 granulepos 算可播时长
    long long gp = 0;
    size_t off = 0;
    while (off + 27 <= p.size() && std::memcmp(p.data() + off, "OggS", 4) == 0) {
        long long g = 0;
        std::memcpy(&g, p.data() + off + 6, 8);
        if (g > 0) gp = g;
        const size_t nseg = p[off + 26];
        if (off + 27 + nseg > p.size()) break;
        size_t blen = 0;
        for (size_t i = 0; i < nseg; ++i) blen += p[off + 27 + i];
        off += 27 + nseg + blen;
    }

    // granulepos 含 pre-skip(单声道 libopusenc 是 312), 可播时长要减掉
    const long long playableMs = (gp > 312 ? gp - 312 : 0) * 1000 / 48000;
    std::printf("         granulepos=%lld -> 可播 %.3fs\n", gp, playableMs / 1000.0);
    CHECK(playableMs > 800 && playableMs < 1600, "可播时长在录的 1 秒附近");

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

    // 播放期间(而不是停播之后)取波形 —— App 就是这么用的: 播放线程先 PlayFile
    // 再 BuildWaveform。顺带验了另开文件流不会打乱正在播放那条流的读写位置
    CheckWaveform(player, wav.data() + sizeof(WavHeader), pcmBytes);
    CHECK(player.IsPlaying(), "取波形不影响播放(还在播)");

    const DWORD t0 = ::GetTickCount();
    while (player.IsPlaying() && ::GetTickCount() - t0 < 1500) ::Sleep(20);   // 差值式, 回绕安全

    CHECK(!player.IsPlaying(), "播完后 IsPlaying 转假");
    CHECK(player.GetPlayPos() >= pcmBytes * 9 / 10, "播放位置推进到接近末尾");
    CHECK(player.GetPlayPosMs() > 400, "按毫秒算也接近末尾");

    player.StopPlay();
    std::remove(path.c_str());
}

/**
 * @brief 播放 .opus / .aopus: 播放器认得出 Opus 分支, 能起播、取波形、自然播完
 * @note 需要声卡。这是"解码器接进播放链"的端到端验证
 */
static void TestPlayOpus() {
    const std::vector<uint8_t> wav = tst::MakeWav(SAMPLE_RATE / 2);   // 0.5 秒
    const size_t pcmBytes = wav.size() - sizeof(WavHeader);

    for (int enc = 0; enc < 2; ++enc) {
        const bool bEnc = (enc == 1);
        const std::string path = bEnc ? "t_play_opus.aopus" : "t_play_opus.opus";
        std::remove(path.c_str());
        {
            COpusStreamWriter w;
            if (w.Open(path.c_str(), bEnc) != AudioSdk::AudioSdkState::NONE) {
                CHECK(false, "播放 Opus: 造测试文件失败");
                continue;
            }
            w.Write(const_cast<uint8_t*>(wav.data()) + sizeof(WavHeader), pcmBytes);
            w.Finish();
        }

        CAudioPlayer player;
        const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
        if (tst::DeviceUnavailable(st)) {
            tst::Skip("播放 Opus", "没有可用播放设备");
            std::remove(path.c_str());
            return;
        }
        CHECK(st == AudioSdk::AudioSdkState::NONE, "播放 Opus: PlayWavFile 返回 NONE");
        if (st != AudioSdk::AudioSdkState::NONE) { std::remove(path.c_str()); continue; }

        const uint32_t totalMs = player.GetTotalPosMs();
        std::printf("         %s 总时长 %u ms\n", bEnc ? "aopus" : "opus", totalMs);
        CHECK(totalMs > 400 && totalMs < 600, "播放 Opus: 总时长 ≈ 500ms");

        // 波形图走的是另开一份解码器, 顺带验一下
        float peaks[CWaveform::kFilePoints * 2] = {};
        player.BuildWaveform([](const float* mm, int n, void* d) {
            if (n > 0) { static_cast<float*>(d)[0] = mm[0]; static_cast<float*>(d)[1] = mm[1]; }
        }, peaks);
        CHECK(peaks[1] > 0.2f, "播放 Opus: 取到了非零波形");

        const DWORD t0 = ::GetTickCount();
        while (player.IsPlaying() && ::GetTickCount() - t0 < 2000) ::Sleep(20);
        CHECK(!player.IsPlaying(), "播放 Opus: 自然播完");
        CHECK(player.GetPlayPos() >= player.GetTotalPos() * 9 / 10, "播放 Opus: 位置推进到接近末尾");

        player.StopPlay();
        std::remove(path.c_str());
    }
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

    // 加密文件的波形要走"分块读 + 按偏移解密"这条路 —— 相位传 0 的话这里立刻对不上
    CheckWaveform(player, wav.data() + sizeof(WavHeader), pcmBytes);

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

// 由 test_main.cpp 依次调用; 返回本套的失败数
int RunPositive() {
    tst::Begin("正向测试");
    RUN(TestWavRoundTrip);
    RUN(TestAencRoundTrip);
    RUN(TestExtraChunkOffsets);
    RUN(TestStreamPlumbing);
    RUN(TestOpusRoundTrip);
    RUN(TestRecordStreamsToDisk);
    RUN(TestRecordPipeline);
    RUN(TestRecordOpus);
    RUN(TestPlayWav);
    RUN(TestPlayOpus);
    RUN(TestPlayAenc);
    RUN(TestRecordThenPlay);
    return tst::Summary("正向") ? 1 : 0;
}
