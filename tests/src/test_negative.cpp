/* @Created On : 2026/9/29
   @Author : 孟源
   @note : 反向测试 —— 非法输入必须被挡住, 且返回码要指明"哪里不对"。

          做法: 先拿 tst::MakeWav 造一个合法文件, 再精确改坏某一个字段。
          这样每次只破坏一处, 能确认到底是哪条规则在拦 —— 比随手丢个乱文件有信息量。

          例外: TestBadFileViaPlayer 会真的走一遍播放器。校验在开设备之前返回,
          所以这部分不需要声卡也能跑。
*/
#include "../include/test_common.h"

#include "audio_sdk/audio_player.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/wav_validate.h"

// ===========================================================================
// RIFF / fmt 块
// ===========================================================================

/**
 * @brief RIFF 魔数被改坏(RIFF→RIFX): 不是认识的文件, 必须拒
 * @note RIFX 是"大端 RIFF"的合法标识, 但本 SDK 只收小端 PCM, 所以它应当被拒
 */
static void TestBadRiffMagic() {
    std::vector<uint8_t> buf = tst::MakeWav(1024);
    std::memcpy(buf.data(), "RIFX", 4);
    WavValidate v;
    CHECK(!v.Validate(buf.data(), buf.size()), "RIFX 魔数被拒绝");

    std::memcpy(buf.data(), "WAVE", 4);      // 干脆把 WAVE 字样搬到开头
    CHECK(!v.Validate(buf.data(), buf.size()), "开头是 WAVE 的字样被拒绝");

    std::memcpy(buf.data(), "riff", 4);      // 大小写不匹配也不认
    CHECK(!v.Validate(buf.data(), buf.size()), "小写 riff 被拒绝");
}

/**
 * @brief riffSize 与文件实际长度不符: 虚报大(文件被截断)和虚报小(混入异物)都要拒
 * @note 这个字段是后面所有越界判断的基准, 错了就得当场拦下
 */
static void TestRiffSizeMismatch() {
    const std::vector<uint8_t> base = tst::MakeWav(1024);

    {   // 虚报大: 声称文件比实际长
        std::vector<uint8_t> buf = base;
        tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size()));
        WavValidate v;
        CHECK(!v.Validate(buf.data(), buf.size()), "riffSize 虚报大被拒绝");
    }
    {   // 虚报小: 声称文件比实际短
        std::vector<uint8_t> buf = base;
        tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8 - 4));
        WavValidate v;
        CHECK(!v.Validate(buf.data(), buf.size()), "riffSize 虚报小被拒绝");
    }
}

/**
 * @brief fmt 块长度不是 16: 当前只支持标准 PCM 的 16 字节 fmt
 * @note 18 字节 fmt 是带 cbSize 的扩展格式, 字段布局不同, 不能硬按 16 读
 */
static void TestFmtSizeNot16() {
    const uint32_t bad[] = { 0, 14, 18, 40 };
    for (uint32_t n : bad) {
        std::vector<uint8_t> buf = tst::MakeWav(1024);
        tst::PutU32(buf, tst::kOffFmtSize, n);
        WavValidate v;
        char msg[64];
        std::snprintf(msg, sizeof(msg), "fmtSize=%u 被拒绝", n);
        CHECK(!v.Validate(buf.data(), buf.size()), msg);
    }
}

/**
 * @brief audioFormat 不是 1(PCM): 压缩格式的数据不能当 PCM 直接播
 */
static void TestNonPcmFormat() {
    const uint16_t bad[] = { 0, 2 /*ADPCM*/, 3 /*IEEE float*/, 0xFFFE /*EXTENSIBLE*/ };
    for (uint16_t n : bad) {
        std::vector<uint8_t> buf = tst::MakeWav(1024);
        tst::PutU16(buf, tst::kOffFormat, n);
        WavValidate v;
        char msg[64];
        std::snprintf(msg, sizeof(msg), "audioFormat=%u 被拒绝", n);
        CHECK(!v.Validate(buf.data(), buf.size()), msg);
    }
}

/**
 * @brief data 子块排在 fmt 之前: 没有格式参数就无从解析, 必须拒
 * @note 这条最容易写漏 —— 只看"偏移 36 是不是 data"的实现会直接把 fmt 头当数据
 */
static void TestDataBeforeFmt() {
    const uint32_t frames   = 256;
    const uint32_t pcmBytes = frames * CHANNELS * (BITS_PER_SAMPLE / 8);

    std::vector<uint8_t> buf(12, 0);
    std::memcpy(buf.data(), "RIFF", 4);
    std::memcpy(buf.data() + 8, "WAVE", 4);

    // data 块(排在前面)
    const size_t dataHdrOff = buf.size();
    buf.resize(buf.size() + 8);
    std::memcpy(buf.data() + dataHdrOff, "data", 4);
    tst::PutU32(buf, dataHdrOff + 4, pcmBytes);
    buf.resize(buf.size() + pcmBytes, 0);

    // fmt 块
    const size_t fmtOff = buf.size();
    buf.resize(buf.size() + 8 + 16, 0);
    std::memcpy(buf.data() + fmtOff, "fmt ", 4);
    tst::PutU32(buf, fmtOff + 4, 16);
    tst::PutU16(buf, fmtOff + 8,  1);
    tst::PutU16(buf, fmtOff + 10, CHANNELS);
    tst::PutU32(buf, fmtOff + 12, SAMPLE_RATE);
    tst::PutU32(buf, fmtOff + 16, SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));
    tst::PutU16(buf, fmtOff + 20, CHANNELS * (BITS_PER_SAMPLE / 8));
    tst::PutU16(buf, fmtOff + 22, BITS_PER_SAMPLE);

    tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8));

    WavValidate v;
    CHECK(!v.Validate(buf.data(), buf.size()), "data 先于 fmt 被拒绝");
}

// ===========================================================================
// 参数自洽性
// ===========================================================================

/**
 * @brief 位深不是 8 的整数倍: 无法按字节切帧, 必须拒
 * @note 故意把 blockAlign/byteRate 也改成"按 12 位算"的假自洽值 ——
 *       这样它被拒就只能是因为 12 % 8 != 0, 而不是被别的规则捎带挡下
 */
static void TestBadBitsPerSample() {
    std::vector<uint8_t> buf = tst::MakeWav(1024);
    const uint16_t align = CHANNELS * (12 / 8);                 // 整数除 → 1
    tst::PutU16(buf, tst::kOffBits, 12);
    tst::PutU16(buf, tst::kOffAlign, align);
    tst::PutU32(buf, tst::kOffByteRate, static_cast<uint32_t>(SAMPLE_RATE) * align);

    WavValidate v;
    CHECK(!v.Validate(buf.data(), buf.size()), "位深 12(非 8 倍数)被拒绝");

    std::vector<uint8_t> b2 = tst::MakeWav(1024);
    tst::PutU16(b2, tst::kOffBits, 0);
    CHECK(!v.Validate(b2.data(), b2.size()), "位深 0 被拒绝");
}

/**
 * @brief blockAlign 与声道数×位深不自洽: 它是后面整帧检查的除数, 错值必须拦下
 */
static void TestBadBlockAlign() {
    const uint16_t bad[] = { 0, 1, 3, 8 };
    for (uint16_t n : bad) {
        std::vector<uint8_t> buf = tst::MakeWav(1024);   // 真实值应为 2
        tst::PutU16(buf, tst::kOffAlign, n);
        WavValidate v;
        char msg[64];
        std::snprintf(msg, sizeof(msg), "blockAlign=%u 被拒绝", n);
        CHECK(!v.Validate(buf.data(), buf.size()), msg);
    }
}

/**
 * @brief 声道数为 0、采样率为 0: 无意义的格式参数, 必须拒
 */
static void TestZeroFormatParams() {
    {
        std::vector<uint8_t> buf = tst::MakeWav(1024);
        tst::PutU16(buf, tst::kOffChannels, 0);
        WavValidate v;
        CHECK(!v.Validate(buf.data(), buf.size()), "声道数 0 被拒绝");
    }
    {
        std::vector<uint8_t> buf = tst::MakeWav(1024);
        tst::PutU32(buf, tst::kOffRate, 0);
        WavValidate v;
        CHECK(!v.Validate(buf.data(), buf.size()), "采样率 0 被拒绝");
    }
}

/**
 * @brief data 长度不是整帧的整数倍: 尾部有半帧残料, 应当拒
 * @note 文件里真的多写 1 字节, 并把 riffSize 一起改自洽 ——
 *       这样它被拒就只能是因为"不是整帧", 而不是被越界规则捎带挡下
 */
static void TestDataNotWholeFrames() {
    std::vector<uint8_t> buf = tst::MakeWav(1024);
    const uint32_t dataSize = static_cast<uint32_t>(buf.size() - sizeof(WavHeader));

    buf.push_back(0);                                        // 文件里真多出 1 字节
    tst::PutU32(buf, tst::kOffDataSize, dataSize + 1);       // 声称数据是奇数长
    tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8));

    WavValidate v;
    CHECK(!v.Validate(buf.data(), buf.size()), "data 长度非整帧被拒绝");
}

/**
 * @brief data 块声明长度超出文件末尾: 典型的截断/虚报, 必须拒
 * @note 这里 riffSize 保持自洽, 专门看它栽在 data 越界这条规则上
 */
static void TestDataSizeOverflow() {
    std::vector<uint8_t> buf = tst::MakeWav(1024);
    tst::PutU32(buf, tst::kOffDataSize, static_cast<uint32_t>(buf.size()));
    WavValidate v;
    CHECK(!v.Validate(buf.data(), buf.size()), "dataSize 超出文件末尾被拒绝");
}

/**
 * @brief 子块长度声明成接近 UINT32_MAX: 必须是"干净地拒", 不能回绕成死循环
 * @note 原来的遍历是 pos += 8 + len + (len & 1)。len = 0xFFFFFFF8 时
 *       8 + len 在 32 位 size_t 上正好回绕成 0, pos 原地不动 → 死循环。
 *       现在改成先比较"剩余多少", 超出就停。64 位上不回绕, 但同样要拒。
 */
static void TestChunkLengthOverflow() {
    const std::vector<uint8_t> base = tst::MakeWav(1024);

    // 在 fmt 前面插一个 JUNK 块, 长度声明成 0xFFFFFFF8(实际没有载荷)
    std::vector<uint8_t> buf(base.begin(), base.begin() + 12);
    const char junkId[4] = {'J', 'U', 'N', 'K'};
    buf.insert(buf.end(), junkId, junkId + 4);
    for (int i = 0; i < 4; ++i) buf.push_back(0xFF);      // len = 0xFFFFFFFF
    buf.insert(buf.end(), base.begin() + 12, base.end());
    tst::PutU32(buf, tst::kOffRiffSize, static_cast<uint32_t>(buf.size() - 8));

    WavValidate v;
    CHECK(!v.Validate(buf.data(), buf.size()), "子块长度 ≈ UINT32_MAX 被拒绝(不会回绕/死循环)");

    // riffSize 回绕: 声称 4GB 的头, 配一个"退绕后正好对得上"的 7 字节总长
    std::vector<uint8_t> buf2 = tst::MakeWav(1024);
    tst::PutU32(buf2, tst::kOffRiffSize, 0xFFFFFFFFu);     // +8 在 uint32 上回绕成 7
    WavValidate v2;
    CHECK(!v2.Validate(buf2.data(), buf2.size(), 7), "riffSize 回绕后不能与 7 字节总长匹配");
}

// ===========================================================================
// 截断 / 加密容器
// ===========================================================================

/**
 * @brief 各种截断长度: 从空文件到"刚好差一字节", 都不能崩, 要干净地拒
 * @note 这些是真实世界最常遇到的: 空文件、写了一半、传输被掐断
 */
static void TestTruncatedSizes() {
    const std::vector<uint8_t> base = tst::MakeWav(1024);
    const size_t lens[] = { 0, 1, 3, 4, 12, 16, 36, 43, 44 };
    for (size_t n : lens) {
        std::vector<uint8_t> buf(base.begin(), base.begin() + n);
        WavValidate v;
        char msg[64];
        std::snprintf(msg, sizeof(msg), "截断到 %zu 字节被拒绝", n);
        CHECK(!v.Validate(buf.data(), buf.size()), msg);
    }
}

/**
 * @brief 截断到"头完整、数据全丢": 头本身自洽, 但 riffSize 与 dataSize 都对不上
 */
static void TestHeaderOnlyTruncation() {
    const std::vector<uint8_t> base = tst::MakeWav(1024);
    std::vector<uint8_t> buf(base.begin(), base.begin() + sizeof(WavHeader));
    WavValidate v;
    CHECK(!v.Validate(buf.data(), buf.size()),
          "只有 44 字节头、数据全无的 WAV 被拒绝");
}

/**
 * @brief 加密容器前缀不完整: 连魔数+版本都凑不齐, 必须拒
 */
static void TestAencPrefixTruncated() {
    {
        const uint8_t m[3] = { 'A', 'E', 'N' };
        CHECK(!CEncryptedFormat::IsAencData(m, 3), "不足 4 字节时 IsAencData 为假");
    }
    {
        const uint8_t m[4] = { 'A', 'E', 'N', 'C' };
        CHECK(CEncryptedFormat::IsAencData(m, 4), "满 4 字节魔数时 IsAencData 为真");
        CHECK(!CEncryptedFormat::IsAencData(m, 3), "同样内容只给 3 字节时为假(长度要守住)");
    }
    {
        const uint8_t m[4] = { 'A', 'E', 'N', 'D' };      // 第四字节不对
        CHECK(!CEncryptedFormat::IsAencData(m, 4), "魔数末位不符为假");
    }
}

/**
 * @brief 加密容器内部 WAV 头损坏: 外壳魔数看着没问题, 剥开后必须校验出问题
 * @note 这条区分了"只认魔数"和"真的会把里面读一遍"—— 只认魔数就漏了
 */
static void TestAencInnerCorrupt() {
    const std::string path = "t_bad_inner.aenc";
    const std::vector<uint8_t> src = tst::MakeWav(1024);
    const size_t pcmBytes = src.size() - sizeof(WavHeader);

    CHECK(CWavFormat::SaveWavFile(path.c_str(), src.data() + sizeof(WavHeader),
                                  pcmBytes, true) == AudioSdk::AudioSdkState::NONE,
          "先造一个合法 .aenc");

    std::vector<uint8_t> aenc = tst::ReadAll(path);
    CHECK(!aenc.empty(), "读回刚写的 .aenc");
    if (aenc.empty()) return;

    // 把内部 WAV 头的 RIFF 改坏(记得跳过 6 字节前缀)
    std::memcpy(aenc.data() + CEncryptedFormat::kAencPrefixSize, "RIFX", 4);
    CHECK(tst::WriteAll(path, aenc), "写回被改坏的文件");

    CHECK(CEncryptedFormat::IsAencFile(path.c_str()),
          "外层魔数仍在, 所以 IsAencFile 仍判为加密容器");

    WavValidate v;
    CHECK(!v.Validate(aenc.data() + CEncryptedFormat::kAencPrefixSize,
                      aenc.size() - CEncryptedFormat::kAencPrefixSize),
          "剥开后内部 WAV 头损坏, 被校验拒绝");

    std::remove(path.c_str());
}

// ===========================================================================
// 走播放器入口
// ===========================================================================

/**
 * @brief 坏文件走播放器入口: 必须在校验阶段返回 FORMAT_NOT_SUPPORTED
 * @note 校验发生在开设备之前, 所以这段不需要声卡
 */
static void TestBadFileViaPlayer() {
    const std::string path = "t_bad_play.bin";
    CAudioPlayer player;

    {   // 魔数错
        std::vector<uint8_t> b = tst::MakeWav(256);
        std::memcpy(b.data(), "RIFX", 4);
        tst::WriteAll(path, b);
        CHECK(player.PlayWavFile(path.c_str()) == AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED,
              "魔数错的 WAV → FORMAT_NOT_SUPPORTED");
    }
    {   // 全 0
        tst::WriteAll(path, std::vector<uint8_t>(256, 0));
        CHECK(player.PlayWavFile(path.c_str()) == AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED,
              "全 0 文件 → FORMAT_NOT_SUPPORTED");
    }
    {   // 只有 3 字节
        tst::WriteAll(path, std::vector<uint8_t>{ 1, 2, 3 });
        CHECK(player.PlayWavFile(path.c_str()) == AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED,
              "3 字节文件 → FORMAT_NOT_SUPPORTED");
    }
    {   // 头合法但 data 声明越界
        std::vector<uint8_t> b = tst::MakeWav(256);
        tst::PutU32(b, tst::kOffDataSize, static_cast<uint32_t>(b.size()));
        tst::WriteAll(path, b);
        CHECK(player.PlayWavFile(path.c_str()) == AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED,
              "dataSize 越界的 WAV → FORMAT_NOT_SUPPORTED");
    }
    {   // 只有 4 字节 AENC 魔数、没有内容
        tst::WriteAll(path, std::vector<uint8_t>{ 'A', 'E', 'N', 'C' });
        CHECK(player.PlayWavFile(path.c_str()) == AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED,
              "只有魔数的 .aenc → FORMAT_NOT_SUPPORTED");
    }

    CHECK(player.PlayWavFile(nullptr) == AudioSdk::AudioSdkState::INVALID_PARAMETER,
          "NULL 路径 → INVALID_PARAMETER");
    CHECK(player.PlayWavFile("no_such_file_xyz.wav") == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
          "不存在的文件 → FILE_OPEN_FAILED");
    CHECK(player.PlayWavFile("") == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
          "空串路径 → FILE_OPEN_FAILED(打不开, 不是参数错)");

    std::remove(path.c_str());
}

/**
 * @brief 扩展名与内容不符: 判定必须看内容(魔数), 不看后缀
 * @note aenc 内容套 .wav 后缀要走通 —— 这正是"用户手动改了扩展名"的场景
 */
static void TestNameDisguise() {
    const std::string path = "t_disguise.wav";     // 后缀是 .wav
    const std::vector<uint8_t> wav = tst::MakeWav(512);
    const size_t pcmBytes = wav.size() - sizeof(WavHeader);

    CHECK(CWavFormat::SaveWavFile(path.c_str(), wav.data() + sizeof(WavHeader),
                                  pcmBytes, true) == AudioSdk::AudioSdkState::NONE,
          "造出 aenc 内容但用 .wav 后缀");

    const std::vector<uint8_t> buf = tst::ReadAll(path);
    CHECK(CEncryptedFormat::IsAencData(buf.data(), buf.size()),
          "后缀是 .wav, IsAencData 仍按内容判为加密");

    CAudioPlayer player;
    const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
    if (tst::DeviceUnavailable(st))
        tst::Skip("扩展名伪装(播放)", "没有可用播放设备");
    else if (st != AudioSdk::AudioSdkState::NONE)
        CHECK(false, "播放器按内容识别 —— 不该因为后缀是 .wav 就失败");
    else {
        CHECK(player.GetTotalPos() == pcmBytes, "解密后总字节数正确");
        player.StopPlay();
    }

    std::remove(path.c_str());
}

/**
 * @brief 保存路径不可写: 必须返回 FILE_OPEN_FAILED, 而不是假装写成功
 * @note 用"指向目录"和"含非法字符"两种一定打不开的路径 —— 比编个不存在的盘符稳,
 *       因为谁也不知道跑测试的机器上到底有没有 Z 盘
 */
static void TestSaveToUnwritablePath() {
    const std::vector<uint8_t> src = tst::MakeWav(256);
    const size_t pcmBytes = src.size() - sizeof(WavHeader);
    const uint8_t* pcm = src.data() + sizeof(WavHeader);

    CHECK(CWavFormat::SaveWavFile(".", pcm, pcmBytes, false)
              == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
          "目标是目录 → FILE_OPEN_FAILED");

    CHECK(CWavFormat::SaveWavFile("t_bad<>name.aenc", pcm, pcmBytes, true)
              != AudioSdk::AudioSdkState::NONE,
          "文件名含非法字符 → 报错");

    CHECK(CWavFormat::SaveWavFile(nullptr, pcm, pcmBytes, false)
              == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
          "路径为 NULL → FILE_OPEN_FAILED");
}

/**
 * @brief 边录边写器: 没起来/起不来的那些情形, 状态要如实, 不能假装写成功了
 * @note 音频线程只看 Write 的 bool 来决定要不要停采 —— 这里报错要是含糊,
 *       录出来的就是"看着有文件、其实是半截"的东西。
 */
static void TestStreamWriterErrors() {
    uint8_t pcm[64] = {};

    // ---- 没 Open 就用: 不能崩, 也不能说成功 ----
    {
        CWavStreamWriter w;
        CHECK(w.GetWritten() == 0, "没 Open 时 Written() 为 0");
        CHECK(w.Write(pcm, sizeof(pcm)) == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
              "没 Open 就 Write → FILE_OPEN_FAILED");
        CHECK(w.Finish(), "没 Open 就 Finish → true(本来就没东西要落)");
    }

    // ---- 路径打不开: Open 要报出来, 且不留半个文件 ----
    {
        CWavStreamWriter w;
        CHECK(w.Open(nullptr, false) == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
              "路径为 NULL → FILE_OPEN_FAILED");
        CHECK(w.Open(".", false) == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
              "目标是目录 → FILE_OPEN_FAILED");
        CHECK(w.Open("no_such_dir_zzz/x.wav", false)
                  == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
              "目录不存在 → FILE_OPEN_FAILED");
        CHECK(w.Write(pcm, sizeof(pcm)) == AudioSdk::AudioSdkState::FILE_OPEN_FAILED,
              "Open 失败之后 Write 仍是 FILE_OPEN_FAILED");
    }

    // ---- 空块不算失败: 设备刚起时给 0 字节的块是正常现象 ----
    {
        const std::string path = "t_writer_empty.wav";
        std::remove(path.c_str());

        CWavStreamWriter w;
        CHECK(w.Open(path.c_str(), false) == AudioSdk::AudioSdkState::NONE, "空录音: Open 成功");
        CHECK(w.Write(pcm, 0) == AudioSdk::AudioSdkState::NONE, "写 0 字节不算失败");
        CHECK(w.GetWritten() == 0, "写 0 字节不改已写字节数");
        CHECK(w.Finish(), "空录音: Finish 成功");

        const std::vector<uint8_t> buf = tst::ReadAll(path);
        CHECK(buf.size() == sizeof(WavHeader), "空录音落盘就是一个只有 44 字节头的 WAV");
        if (buf.size() == sizeof(WavHeader)) {
            WavValidate v;
            CHECK(v.Validate(buf.data(), buf.size()), "空 WAV 的头是合法的(长度 0)");
        }
        std::remove(path.c_str());
    }
}

/**
 * @brief 写真的失败时, Write 必须报出来 —— 不能静默返回"成功"
 *
 * 这条是给一个反复踩到的坑站岗: ofstream 默认 exceptions()==0, write 失败【不抛异常】,
 * 只把 badbit 置起来。于是 "try/catch 包住 write" 和 "write 后无条件 throw" 两种写法
 * 都能让这段代码坏掉 —— 前者永远报成功, 后者永远报失败。只有 good() 是对的。
 *
 * 造失败的手法: 把文件开在一个"服务端已关"的命名管道上。它和盘满(ENOSPC)在 CRT 里
 * 走同一条 _Fwrite 失败路径, 流层反应完全一致, 不用真去填满一块盘。
 */
static void TestWriteFailureReported() {
#if defined(_WIN32)
    const wchar_t* kPipe = L"\\\\.\\pipe\\wav_writer_test";
    HANDLE hSrv = ::CreateNamedPipeW(kPipe, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (hSrv == INVALID_HANDLE_VALUE) { tst::Skip("写失败上报", "建命名管道失败"); return; }

    CWavStreamWriter w;
    // 路径是 ASCII, u8path 不涉及转码; Open 时管道还在, 占位头能写进去
    const AudioSdk::AudioSdkState st = w.Open("\\\\.\\pipe\\wav_writer_test", false);
    if (st != AudioSdk::AudioSdkState::NONE) {
        ::CloseHandle(hSrv);
        tst::Skip("写失败上报", "打不开管道");
        return;
    }
    ::CloseHandle(hSrv);              // 服务端一关, 写端此后每次 WriteFile 都失败

    uint8_t pcm[8192];
    std::memset(pcm, 0xAA, sizeof(pcm));

    // 反复写直到失败: ofstream 有缓冲, 头几次可能还在缓冲里没落到底层
    bool reported = false;
    size_t tries = 0;
    for (; tries < 64; ++tries) {
        if (w.Write(pcm, sizeof(pcm)) != AudioSdk::AudioSdkState::NONE) { reported = true; break; }
    }
    CHECK(reported, "写不进去时 Write 报出错(不是静默成功)");
    CHECK(tries < 64, "失败在头几次写内就被发现(不是拖到最后)");
    CHECK(w.GetWritten() == tries * sizeof(pcm),
          "失败那一块没有被计进 Written() —— 字节数不能凭空涨");
#else
    std::printf("  [skip] 命名管道是 Windows 专属\n");
#endif
}

// 由 test_main.cpp 依次调用; 返回本套的失败数
int RunNegative() {
    tst::Begin("反向测试");
    RUN(TestBadRiffMagic);
    RUN(TestRiffSizeMismatch);
    RUN(TestFmtSizeNot16);
    RUN(TestNonPcmFormat);
    RUN(TestDataBeforeFmt);
    RUN(TestBadBitsPerSample);
    RUN(TestBadBlockAlign);
    RUN(TestZeroFormatParams);
    RUN(TestDataNotWholeFrames);
    RUN(TestDataSizeOverflow);
    RUN(TestChunkLengthOverflow);
    RUN(TestTruncatedSizes);
    RUN(TestHeaderOnlyTruncation);
    RUN(TestAencPrefixTruncated);
    RUN(TestAencInnerCorrupt);
    RUN(TestBadFileViaPlayer);
    RUN(TestNameDisguise);
    RUN(TestSaveToUnwritablePath);
    RUN(TestStreamWriterErrors);
    RUN(TestWriteFailureReported);
    return tst::Summary("反向") ? 1 : 0;
}
