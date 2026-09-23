/* @Created On : 2026/9/22
   @Author : 孟源
   @note : 格式层边界单测 —— WavValidate / CWavFormat / CEncryptedFormat

          只在内存和 %TEMP% 里干活: 不碰音频设备、不碰线程、不依赖 SDK 其余部分,
          静态链格式层三个 .cpp 就能跑, 秒级完成。可以日常反复跑, 也可以进 CI。

          测试哲学对齐 web 接口测试: 换参数、打边界 —— 只是"参数"变成了字节流,
          "响应码"变成了返回值/状态码。每一条都对应一类真实损坏或真实素材形态。
*/
#include "audio_sdk/wav_validate.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/encrypted_format.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------- 极简断言设施 ----------------

static int g_total = 0;
static int g_fail  = 0;

#define CHECK(cond)    DoCheck(!!(cond), #cond, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) DoCheck(!!(cond), msg,   __FILE__, __LINE__)

/**
 * @brief 断言公共落点: 计数, 失败时打印表达式原文与所在位置
 * @param ok 断言是否成立
 * @param what 失败时打印的内容(CHECK 展开出的表达式原文或自定义消息)
 * @param file 断言所在文件
 * @param line 断言所在行号
 */
static void DoCheck(bool ok, const char* what, const char* file, int line){
    ++g_total;
    if (!ok){
        ++g_fail;
        std::printf("    [FAIL] %s  (%s:%d)\n", what, file, line);
    }
}

// ---------------- 工具 ----------------

/**
 * @brief 用 SDK 自己的 FillHeader 造一个合法的 44 字节头 + data 的标准 WAV 缓冲
 * @param channels 声道数
 * @param rate 采样率
 * @param bits 位深
 * @param dataBytes PCM 数据字节数
 * @return 完整 WAV 文件缓冲(数据填非零 0xEE, 防"全零恰好合法"的假通过)
 */
static std::vector<uint8_t> MakeStdWav(uint16_t channels, uint32_t rate,
                                       uint16_t bits, uint32_t dataBytes){
    WavHeader h;
    CWavFormat::FillHeader(h, dataBytes, rate, channels, bits);
    std::vector<uint8_t> buf(sizeof(WavHeader) + dataBytes, 0xEE);   // 数据填非零
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

/**
 * @brief 在标准 WAV 里插入一个任意子块(自动补奇数填充), 并修正 riffSize
 *        —— 造出"带额外子块"的合法文件
 * @param stdWav MakeStdWav 产出的标准 WAV
 * @param id4 子块 ID(恰好 4 字符)
 * @param payload 子块载荷
 * @param at 插入偏移: 36 = fmt 之后(默认, 常见形态); 12 = "WAVE" 之后、fmt 之前
 *        (JUNK/bext 前置形态)
 * @return 插好子块的完整文件缓冲
 * @note at=36 时必须把标准头里 dataId/dataSize 那两个字段从中间挖走 ——
 *       真实世界的带子块文件不存在"残留的旧 data 头", 别造成那种形态。
 *       at=12 时那段(36..44)本来就不是 data 头(快照里是 fmt 尾+data 头整体后移),
 *       直接把 12 之后的内容整体接在新块后面即可。
 */
static std::vector<uint8_t> InsertChunk(const std::vector<uint8_t>& stdWav,
                                        const char* id4, const std::vector<uint8_t>& payload,
                                        size_t at = 36){
    const size_t dataLen  = stdWav.size() - sizeof(WavHeader);
    const size_t pad      = payload.size() & 1;

    std::vector<uint8_t> out;
    out.reserve(at + 8 + payload.size() + pad + (stdWav.size() - at));
    out.assign(stdWav.begin(), stdWav.begin() + at);                // 头部到插入点为止
    out.insert(out.end(), id4, id4 + 4);
    const uint32_t len = static_cast<uint32_t>(payload.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&len),
               reinterpret_cast<const uint8_t*>(&len) + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    out.resize(out.size() + pad);                                    // 奇数长度的填充字节
    // 插入点之后的原内容原样接上(fmt 或 data 都行)
    out.insert(out.end(), stdWav.begin() + at, stdWav.end());

    // 修正 riffSize: 文件总长 - 8
    const uint32_t riff = static_cast<uint32_t>(out.size()) - 8;
    std::memcpy(out.data() + 4, &riff, 4);
    return out;
}

/**
 * @brief 把缓冲里 off 处的 32 位整数字段改成 v(小端, 直接内存拷) —— 破坏头字段用
 */
static void PatchU32(std::vector<uint8_t>& buf, size_t off, uint32_t v){
    std::memcpy(buf.data() + off, &v, 4);
}

/**
 * @brief 同 PatchU32, 改的是 16 位字段
 */
static void PatchU16(std::vector<uint8_t>& buf, size_t off, uint16_t v){
    std::memcpy(buf.data() + off, &v, 2);
}

// WavHeader 字段偏移(和 wav_format.h 的 pack(1) 结构一一对应)
namespace off {
    constexpr size_t kRiffSize      = 4;
    constexpr size_t kFmtSize       = 16;
    constexpr size_t kAudioFormat   = 20;
    constexpr size_t kNumChannels   = 22;
    constexpr size_t kSampleRate    = 24;
    constexpr size_t kByteRate      = 28;
    constexpr size_t kBlockAlign    = 32;
    constexpr size_t kBitsPerSample = 34;
    constexpr size_t kDataId        = 36;
    constexpr size_t kDataSize      = 40;
}

/**
 * @brief 取系统 %TEMP% 当落盘目录 —— 每个用例自己起文件名, 结束统一删
 * @return 临时目录路径(带尾反斜杠)
 */
static std::wstring TempDir(){
    wchar_t p[MAX_PATH];
    ::GetTempPathW(MAX_PATH, p);
    return p;
}

/**
 * @brief 删测试落盘的临时文件, 不关心它是否存在、是否删得掉
 */
static void DeleteFileQuiet(const std::wstring& path){ ::DeleteFileW(path.c_str()); }

/**
 * @brief 整文件读进内存(落盘回读用例用)
 * @param path 文件路径
 * @return 文件全部字节; 打不开或空文件返回空缓冲
 */
static std::vector<uint8_t> ReadAll(const std::wstring& path){
    std::vector<uint8_t> buf;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return buf;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf.resize(n > 0 ? static_cast<size_t>(n) : 0);
    if (!buf.empty())
        std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return buf;
}

// ---------------- 1) WavValidate: 标准文件与遍历 ----------------

/**
 * @brief 标准排布用例组: 合法参数组合 / 空数据文件 / 逐字节截断扫描
 * @note 合法组合校验通过后还要核对 DataOffset=44 与头字段回读;
 *       截断扫描从满长度截到 0, 只有完整文件该通过(riffSize 是字段, 截短必对不上)。
 */
static void TestValidator_Standard(){
    std::printf("[1] WavValidate: 标准排布\n");

    // 1.1 各种合法参数组合都应该过, 且 DataOffset 恒为 44、DataSize 与请求数据一致
    struct Combo { uint16_t ch; uint32_t rate; uint16_t bits; };
    const Combo combos[] = {
        {1, 44100, 16}, {2, 44100, 16}, {1, 8000, 16},
        {2, 48000, 24}, {1, 22050, 8},
    };
    // data 长度取各组合帧长的公倍数(2ch/24bit 帧长 6, 其它都是 1/2/4), 保证是帧整数倍
    constexpr uint32_t kDataBytes = 96;
    for (const Combo& c : combos){
        std::vector<uint8_t> buf = MakeStdWav(c.ch, c.rate, c.bits, kDataBytes);
        WavValidate v;
        const bool ok = v.Validate(buf.data(), buf.size());
        CHECK_MSG(ok, "合法参数组合应通过");
        if (ok){
            CHECK(v.GetDataOffset() == sizeof(WavHeader));
            CHECK(v.GetDataSize() == kDataBytes);
            CHECK(v.Header().numChannels == c.ch);
            CHECK(v.Header().sampleRate  == c.rate);
            CHECK(v.Header().bitsPerSample == c.bits);
        }
    }

    // 1.2 零数据(dataSize=0)也算合法文件 —— 空录音
    {
        std::vector<uint8_t> buf = MakeStdWav(1, 44100, 16, 0);
        WavValidate v;
        CHECK(v.Validate(buf.data(), buf.size()));
    }

    // 1.3 逐字节截断: 从满长度截到 0, 只有 >= 44 且结构完整的才是真的
    {
        std::vector<uint8_t> full = MakeStdWav(1, 44100, 16, 64);
        int passCount = 0;
        for (size_t cut = 0; cut <= full.size(); ++cut){
            WavValidate v;
            if (v.Validate(full.data(), cut)) ++passCount;
        }
        // 完整文件过一次; riffSize 是字段, 截短后必然对不上 —— 只应有这 1 个通过
        CHECK_MSG(passCount == 1, "截断变体中只有完整文件通过");
    }
}

/**
 * @brief 头字段边界用例组: 四个魔数逐字节破坏 / fmt 参数矛盾 / riffSize 虚报 / dataSize 撒谎
 * @note 每种破坏都必须被拒 —— 校验器对"结构对但值不讲理"的文件一律返回 false。
 */
static void TestValidator_HeaderFields(){
    std::printf("[2] WavValidate: 头字段边界\n");

    // 魔数四连: 每个 ID 换成一个字节都该被拒
    const size_t idOffs[]   = {0, 8, 12, 36};
    const char*  idNames[]  = {"RIFF", "WAVE", "fmt ", "data"};
    for (int i = 0; i < 4; ++i){
        std::vector<uint8_t> buf = MakeStdWav(1, 44100, 16, 64);
        buf[idOffs[i]] = 'X';
        WavValidate v;
        CHECK_MSG(!v.Validate(buf.data(), buf.size()), idNames[i]);
    }

    // fmt 参数矛盾/非法
    struct Bad { size_t off; bool is32; uint32_t v32; uint16_t v16; const char* what; };
    const Bad bads[] = {
        {off::kFmtSize,       true,  17,    0,   "fmtSize != 16"},
        {off::kAudioFormat,   false, 0,     3,   "audioFormat = float"},
        {off::kNumChannels,   false, 0,     0,   "声道数 = 0"},
        {off::kSampleRate,    true,  0,     0,   "采样率 = 0"},
        {off::kBitsPerSample, false, 0,     0,   "位深 = 0"},
        {off::kBitsPerSample, false, 0,     12,  "位深非字节倍数"},
        {off::kBlockAlign,    false, 0,     999, "blockAlign 与声道矛盾"},
        {off::kByteRate,      true,  999,   0,   "byteRate 与头矛盾"},
    };
    for (const Bad& b : bads){
        std::vector<uint8_t> buf = MakeStdWav(1, 44100, 16, 64);
        if (b.is32) PatchU32(buf, b.off, b.v32); else PatchU16(buf, b.off, b.v16);
        WavValidate v;
        CHECK_MSG(!v.Validate(buf.data(), buf.size()), b.what);
    }

    // riffSize 与文件实际大小不符(短了 = 截断, 长了 = 虚报)
    {
        std::vector<uint8_t> buf = MakeStdWav(1, 44100, 16, 64);
        PatchU32(buf, off::kRiffSize, static_cast<uint32_t>(buf.size()) - 9);
        WavValidate v;
        CHECK_MSG(!v.Validate(buf.data(), buf.size()), "riffSize 偏小");

        buf = MakeStdWav(1, 44100, 16, 64);
        PatchU32(buf, off::kRiffSize, static_cast<uint32_t>(buf.size()) - 7);
        WavValidate v2;
        CHECK_MSG(!v2.Validate(buf.data(), buf.size()), "riffSize 偏大");
    }

    // dataSize 超出文件末尾 / 非帧整数倍
    {
        std::vector<uint8_t> buf = MakeStdWav(1, 44100, 16, 64);
        PatchU32(buf, off::kDataSize, 66);            // 66 > 余量 64, 偶数且帧对齐 —— 只能被越界检查拒
        WavValidate v;
        CHECK_MSG(!v.Validate(buf.data(), buf.size()), "dataSize 超出文件末尾");
    }
}

/**
 * @brief 额外子块用例组(本轮 WavValidate 重修的核心场景)
 * @note 覆盖: 单个 LIST(用户那两个真实素材的形态)、多子块 + 奇数长度填充、
 *       子块长度虚报的安全拒绝、无子块时 DataOffset 恒为 44。
 */
static void TestValidator_ExtraChunks(){
    std::printf("[3] WavValidate: 额外子块(本轮修的核心)\n");

    // 3.1 单个 LIST 子块 —— 就是用户那两个文件的真实形态
    {
        std::vector<uint8_t> stdWav = MakeStdWav(2, 44100, 16, 128);
        const uint8_t listBody[] = {'I','N','F','O','I','S','F','T','x','y'};
        std::vector<uint8_t> withList =
            InsertChunk(stdWav, "LIST", std::vector<uint8_t>(listBody, listBody + 10));
        WavValidate v;
        const bool ok = v.Validate(withList.data(), withList.size());
        CHECK_MSG(ok, "带 LIST 的立体声 WAV 应通过");
        if (ok){
            CHECK(v.GetDataOffset() == 44 + 8 + 10);       // 44 + 块头8 + 载荷10
            CHECK(v.Header().numChannels == 2);
        }
    }

    // 3.2 多个子块 + 奇数长度填充字节
    {
        std::vector<uint8_t> stdWav = MakeStdWav(1, 44100, 16, 64);
        std::vector<uint8_t> buf =
            InsertChunk(InsertChunk(stdWav, "fact", {1,2,3}), "cue ", {'a'});   // 3字节奇数 + 1字节奇数
        WavValidate v;
        const bool ok = v.Validate(buf.data(), buf.size());
        CHECK_MSG(ok, "多子块 + 奇数填充应通过");
        if (ok){
            // fact: 8+3+1=12; cue : 8+1+1=10 → 44+12+10 = 66
            CHECK(v.GetDataOffset() == 66);
        }
    }

    // 3.3 子块宣称的长度超过文件剩余 → 不该疯跳, 应安全拒绝
    {
        std::vector<uint8_t> stdWav = MakeStdWav(1, 44100, 16, 64);
        std::vector<uint8_t> buf = InsertChunk(stdWav, "LIST", {'a','b'});
        PatchU32(buf, 44 + 4, 0x7FFFFFFF);                 // 把 LIST 的长度字段改成天文数字
        WavValidate v;
        CHECK_MSG(!v.Validate(buf.data(), buf.size()), "子块长度虚报应拒绝");
    }

    // 3.4 没有子块直接是 data —— GetDataOffset 仍为 44
    {
        std::vector<uint8_t> buf = MakeStdWav(2, 48000, 24, 96);
        WavValidate v;
        CHECK(v.Validate(buf.data(), buf.size()));
        CHECK(v.GetDataOffset() == 44);
    }

    // 3.5 fmt 前面有前置子块(JUNK/bext 形态): fmt 不在偏移 12, 快照全是别的块
    {
        // 偶数长度 JUNK(8 字节): fmt 块头在 12+8+8=28, "data" 头在 28+8+16=52, 载荷 60
        std::vector<uint8_t> stdWav = MakeStdWav(1, 44100, 16, 64);
        std::vector<uint8_t> buf = InsertChunk(stdWav, "JUNK", {'1','2','3','4','5','6','7','8'}, 12);
        WavValidate v;
        const bool ok = v.Validate(buf.data(), buf.size());
        CHECK_MSG(ok, "前置 JUNK(偶数) 的 WAV 应通过");
        if (ok){
            CHECK(v.GetDataOffset() == 60);
            CHECK(v.Header().sampleRate == 44100);         // 回填后参数必须是真的
            CHECK(v.Header().numChannels == 1);
        }
    }
    {
        // 奇数长度 JUNK(3 字节, 带 1 字节填充): fmt 块头在 12+8+3+1=24, "data" 头在 48, 载荷 56
        std::vector<uint8_t> stdWav = MakeStdWav(2, 48000, 24, 96);
        std::vector<uint8_t> buf = InsertChunk(stdWav, "JUNK", {'a','b','c'}, 12);
        WavValidate v;
        const bool ok = v.Validate(buf.data(), buf.size());
        CHECK_MSG(ok, "前置 JUNK(奇数, 填充对齐) 的 WAV 应通过");
        if (ok){
            CHECK(v.GetDataOffset() == 56);
            CHECK(v.Header().bitsPerSample == 24);
        }
    }

    // 3.6 data 先于 fmt —— 规范禁止, 必须拒绝
    {
        std::vector<uint8_t> stdWav = MakeStdWav(1, 44100, 16, 64);
        std::vector<uint8_t> buf = InsertChunk(stdWav, "data", {'x','y'}, 12);   // data 在前
        WavValidate v;
        CHECK_MSG(!v.Validate(buf.data(), buf.size()), "data 先于 fmt 应拒绝");
    }

    // 3.7 data 后面再垫尾块(LIST/ID3 形态) —— data 不必顶到 EOF, 应通过且长度正确
    {
        std::vector<uint8_t> stdWav = MakeStdWav(1, 44100, 16, 128);
        std::vector<uint8_t> trailing;
        trailing.insert(trailing.end(), stdWav.begin(),
                        stdWav.begin() + sizeof(WavHeader) + 128);   // 标准 44 头 + 128 数据
        const char tailId[4] = {'L','I','S','T'};
        const uint32_t tailLen = 6;
        trailing.insert(trailing.end(), tailId, tailId + 4);
        trailing.insert(trailing.end(),
                        reinterpret_cast<const uint8_t*>(&tailLen),
                        reinterpret_cast<const uint8_t*>(&tailLen) + 4);
        const uint8_t tailBody[6] = {'I','N','F','O','!','!'};
        trailing.insert(trailing.end(), tailBody, tailBody + 6);                 // 偶数长, 无填充
        const uint32_t riff = static_cast<uint32_t>(trailing.size()) - 8;
        std::memcpy(trailing.data() + 4, &riff, 4);

        WavValidate v;
        const bool ok = v.Validate(trailing.data(), trailing.size());
        CHECK_MSG(ok, "data 后垫尾块应通过");
        if (ok){
            CHECK(v.GetDataOffset() == 44);
            CHECK(v.GetDataSize() == 128);                 // 长度必须按 data 块头, 不能按文件余量(134+6)
        }
    }

    // 3.8 data 长度越界(data 后没内容却宣称更长) —— 仍要拒绝
    {
        std::vector<uint8_t> buf = MakeStdWav(1, 44100, 16, 64);
        PatchU32(buf, off::kDataSize, 128);                // 载荷只有 64, 宣称 128
        // 注意: 遍历读的是块头里 data 块自己的 len 字段(偏移 40), 上面的 kDataSize 恰好就是它
        WavValidate v;
        CHECK_MSG(!v.Validate(buf.data(), buf.size()), "data 长度越界应拒绝");
    }
}

// ---------------- 2) CWavFormat: 落盘 + 回读 ----------------

/**
 * @brief 落盘回读闭环: SaveWavFile 存出的明文 WAV 必须能过自家校验, 参数回得来
 * @note 附带 NULL 路径 / 空 PCM 的状态码边界 —— 路径判空是本单测抓出来的真缺陷
 *       (u8path(nullptr) 原本是未定义行为), 这组就是它的回归。
 */
static void TestSaveAndValidate(){
    std::printf("[4] CWavFormat: 落盘回读闭环\n");
    const std::wstring dir = TempDir();
    const std::wstring wavPath = dir + L"sdkfmt_test.wav";

    // 4.1 保存的明文文件必须能通过自家校验(参数也回得来)
    {
        std::vector<int16_t> pcm(4410, 1234);          // 0.1 秒单声道
        const std::string utf8(1, '\0');
        // 窄转 UTF-8: 路径全是 ASCII, 直接逐字节拷
        std::string pathA(wavPath.begin(), wavPath.end());
        const AudioSdk::AudioSdkState st =
            CWavFormat::SaveWavFile(pathA.c_str(), pcm.data(), pcm.size() * 2);
        CHECK(st == AudioSdk::AudioSdkState::NONE);

        std::vector<uint8_t> disk = ReadAll(wavPath);
        CHECK(disk.size() == 44 + pcm.size() * 2);

        WavValidate v;
        CHECK(v.Validate(disk.data(), disk.size()));
        CHECK(v.Header().sampleRate == 44100);
        CHECK(v.Header().numChannels == 1);
        CHECK(v.Header().bitsPerSample == 16);
    }

    // 4.2 NULL / 空 PCM 也得是明确状态码而不是崩
    {
        const std::string pathA(wavPath.begin(), wavPath.end());
        CHECK(CWavFormat::SaveWavFile(pathA.c_str(), nullptr, 0)
              == AudioSdk::AudioSdkState::NONE);        // 0 字节合法(空录音)
        CHECK(CWavFormat::SaveWavFile(nullptr, nullptr, 0)
              == AudioSdk::AudioSdkState::FILE_OPEN_FAILED);   // 路径为 NULL 打不开
    }

    DeleteFileQuiet(wavPath);
}

// ---------------- 3) CEncryptedFormat: .aenc 容器 ----------------

/**
 * @brief .aenc 容器用例组: 保存→识别→解密回读 / XOR 自逆性 / 边界输入
 * @note 5.1 走一遍播放器的完整分流路径(剥前缀→XOR→内部 WAV 校验→位级比对);
 *       5.2 加密两遍等于没加密, 且 16 字节密钥周期可见;
 *       5.3 太短、魔数错一位、NULL 参数都给明确结果而不是崩。
 */
static void TestAenc(){
    std::printf("[5] CEncryptedFormat: .aenc 容器\n");
    const std::wstring dir = TempDir();
    const std::wstring aencPath = dir + L"sdkfmt_test.aenc";
    const std::string pathA(aencPath.begin(), aencPath.end());

    // 5.1 加密保存 → IsAencFile 认出 → 头后密文 XOR 回来必须等于明文
    {
        std::vector<uint8_t> pcm(2048);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(i * 7);

        CHECK(CEncryptedFormat::SaveAencFile(pathA.c_str(), pcm.data(), pcm.size())
              == AudioSdk::AudioSdkState::NONE);
        CHECK(CEncryptedFormat::IsAencFile(pathA.c_str()));

        std::vector<uint8_t> disk = ReadAll(aencPath);
        CHECK(disk.size() == CEncryptedFormat::kAencPrefixSize + 44 + pcm.size());
        CHECK(CEncryptedFormat::IsAencData(disk.data(), disk.size()));

        // 剥前缀 + 解密 + 校验内部头 —— 播放器的完整分流路径
        CEncryptedFormat::XorCrypt(disk.data() + CEncryptedFormat::kAencPrefixSize + 44,
                                   pcm.size());
        WavValidate v;
        CHECK(v.Validate(disk.data() + CEncryptedFormat::kAencPrefixSize,
                         disk.size() - CEncryptedFormat::kAencPrefixSize));
        CHECK(std::memcmp(disk.data() + CEncryptedFormat::kAencPrefixSize + 44,
                          pcm.data(), pcm.size()) == 0);
    }

    // 5.2 XOR 自逆性: 加密两遍 == 没加密
    {
        std::vector<uint8_t> buf(100, 0xAB);
        std::vector<uint8_t> ref = buf;
        CEncryptedFormat::XorCrypt(buf.data(), buf.size());
        CHECK(buf != ref);                                  // 确实变过了
        CEncryptedFormat::XorCrypt(buf.data(), buf.size());
        CHECK(buf == ref);                                  // 又变回来了
        // 16 字节密钥周期: 相差 16 的两处密文相同
        std::vector<uint8_t> t(32, 0);
        CEncryptedFormat::XorCrypt(t.data(), t.size());
        CHECK(std::memcmp(t.data(), t.data() + 16, 16) == 0);
    }

    // 5.3 边界: 太短 / 魔数错一位 / NULL
    {
        CHECK(!CEncryptedFormat::IsAencData(nullptr, 100));
        const uint8_t m[4] = {'A','E','N','C'};
        CHECK(CEncryptedFormat::IsAencData(m, 4));
        CHECK(!CEncryptedFormat::IsAencData(m, 3));         // 差一个字节都不行
        const uint8_t bad[4] = {'A','E','N','X'};
        CHECK(!CEncryptedFormat::IsAencData(bad, 4));

        CHECK(!CEncryptedFormat::IsAencFile(nullptr));
        CHECK(!CEncryptedFormat::IsAencFile("Z:\\no\\such\\file.aenc"));

        CHECK(CEncryptedFormat::SaveAencFile(nullptr, m, 4)
              == AudioSdk::AudioSdkState::INVALID_PARAMETER);
        CHECK(CEncryptedFormat::SaveAencFile(pathA.c_str(), nullptr, 4)
              == AudioSdk::AudioSdkState::INVALID_PARAMETER);
        CHECK(CEncryptedFormat::SaveAencFile(pathA.c_str(), nullptr, 0)
              == AudioSdk::AudioSdkState::NONE);            // 0 字节空容器也该能写
    }

    DeleteFileQuiet(aencPath);
}

// ---------------- 4) 短小文件: 长度边界 ----------------

/**
 * @brief 极短缓冲用例组: 0 到完整长度逐个喂给 Validate, 不许崩、判定要准
 * @note 只有完整 44+data 结构通过; NULL 缓冲两种长度都必须安全拒绝。
 */
static void TestTinyBuffers(){
    std::printf("[6] 极短缓冲\n");
    // 所有 0..47 长度的缓冲都不能崩, 且只有完整 44+ 结构才通过
    const std::vector<uint8_t> full = MakeStdWav(1, 44100, 16, 8);
    for (size_t n = 0; n <= sizeof(WavHeader) + 8; ++n){
        WavValidate v;
        const bool ok = v.Validate(full.data(), n);
        CHECK_MSG(n == full.size() ? ok : !ok, "长度边界判定");
        if (!ok) break;                                 // 一处不符即停, 输出省空间
    }
    // NULL 缓冲
    {
        WavValidate v;
        CHECK(!v.Validate(nullptr, 44));
        CHECK(!v.Validate(nullptr, 0));
    }
}

/**
 * @brief 入口: 依次跑六组用例, 汇总断言计数
 * @param argc/argv 命令行参数: 双击启动时没有参数, 结束会等回车再关窗口;
 *                  带任意参数(脚本/CI)启动时跑完立即退出, 退出码才有意义
 * @return 0 = 全部通过; 1 = 有失败
 */
int main(int argc, char** argv){
    // 源码/执行字符集都是 UTF-8(/utf-8), 但双击运行的控制台默认代码页是 GBK(936),
    // UTF-8 中文会被解成乱码 —— 启动时先把输出代码页切到 UTF-8。
    ::SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲: 崩了也能看到崩在哪一组
    TestValidator_Standard();
    TestValidator_HeaderFields();
    TestValidator_ExtraChunks();
    TestSaveAndValidate();
    TestAenc();
    TestTinyBuffers();

    std::printf("\n%d 项断言, 失败 %d 项 —— %s\n",
                g_total, g_fail, g_fail == 0 ? "PASS" : "FAIL");
    // 双击(explorer)启动的场景没有命令行参数: 停一下, 让人看清结果再关窗
    if (argc <= 1){
        std::printf("\n按回车键退出...");
        std::getchar();
    }
    return g_fail == 0 ? 0 : 1;
}
