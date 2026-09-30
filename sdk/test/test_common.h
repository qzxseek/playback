/* @Created On : 2026/9/29
   @Author : 孟源
   @note : 测试公共件 —— 断言 + 造数据, 不引第三方框架。

          约定(四个测试文件都按这个写):
            每个测试是一个 static void 函数, 函数上方注释写明"测什么";
            main 里用 RUN(函数名) 依次调用 —— RUN 会打印函数名, 所以名字本身要能看懂;
            CHECK(条件, "说明") 逐条断言, 失败的会累计到最后的 Summary。

          需要麦克风/声卡的用例, 在 StartRecording/PlayWavFile 返回设备类错误时
          打印 [skip] 而不是判失败 —— 没设备不该算 SDK 的错。机器有设备时它们会真跑。
*/
#pragma once

#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/audio_types.h"
#include "audio_sdk/wav_format.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>   // 控制台码页 / "是不是双击启动的"判定
#  include <io.h>        // _isatty
#endif

namespace tst {

inline int g_pass = 0;
inline int g_fail = 0;
inline int g_skip = 0;

inline void Report(const char* msg, bool ok, int line, const std::string& detail = {}) {
    if (ok) { ++g_pass; std::printf("  [ok]   %s\n", msg); return; }
    ++g_fail;
    std::printf("  [FAIL] %s   (第 %d 行)\n", msg, line);
    if (!detail.empty()) std::printf("         -> %s\n", detail.c_str());
}

inline void Skip(const char* msg, const char* why) {
    ++g_skip;
    std::printf("  [skip] %s  (%s)\n", msg, why);
}

// 每个 main 的第一句。stdout 默认是块缓冲 —— 异常套件里真有"把活着的对象拆掉"的用例,
// 一旦它把进程带崩, 缓冲区里已经跑过的结果会一起丢掉。
inline void Begin(const char* suite) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("==== %s ====\n", suite);
}

// 测试结束前, 暂停一下, 等用户按回车键
// 从已有 shell 里跑、或输出被重定向时都不停 —— 否则脚本和 CI 会被永久挂住
inline void PauseBeforeExit() {
#if defined(_WIN32)
    DWORD pids[2] = {};
    if (GetConsoleProcessList(pids, 2) != 1) return;
    if (!_isatty(_fileno(stdout))) return;
    std::printf("\n按回车键关闭...");
    std::fflush(stdout);
    std::getchar();
#endif
}

inline int Summary(const char* suite) {
    std::printf("\n==== %s: 通过 %d, 失败 %d, 跳过 %d ====\n",
                suite, g_pass, g_fail, g_skip);
    PauseBeforeExit();
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 造数据 / 读文件
// ---------------------------------------------------------------------------

// 读整个文件; 打不开返回空
inline std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const std::streamsize n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(buf.data()), n);
    if (f.gcount() != n) return {};
    return buf;
}

// 原样写字节(造损坏文件用)
inline bool WriteAll(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::out | std::ios::binary);
    if (!f.is_open()) return false;
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    return f.good();
}

// 按小端改包头字段(造各种不自洽的头用)
inline void PutU16(std::vector<uint8_t>& buf, size_t off, uint16_t v) {
    if (off + 2 > buf.size()) return;
    buf[off + 0] = static_cast<uint8_t>(v & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
inline void PutU32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    if (off + 4 > buf.size()) return;
    buf[off + 0] = static_cast<uint8_t>(v & 0xFF);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// 各字段在 WavHeader 里的偏移。写死在测试里, 顺手当"头布局没被改过"的哨兵。
enum : size_t {
    kOffRiffSize = 4,
    kOffFmtSize  = 16,
    kOffFormat   = 20,
    kOffChannels = 22,
    kOffRate     = 24,
    kOffByteRate = 28,
    kOffAlign    = 32,
    kOffBits     = 34,
    kOffDataSize = 40,
};

// 造一个合法的 PCM WAV(内存态), 当各用例的基准素材
inline std::vector<uint8_t> MakeWav(uint32_t frames,
                                    uint32_t sampleRate = SAMPLE_RATE,
                                    uint16_t channels   = CHANNELS,
                                    uint16_t bits       = BITS_PER_SAMPLE) {
    const uint32_t blockAlign = static_cast<uint32_t>(channels) * (bits / 8);
    const size_t   dataSize   = static_cast<size_t>(frames) * blockAlign;
    std::vector<uint8_t> buf(sizeof(WavHeader) + dataSize, 0);

    WavHeader hdr{};
    CWavFormat::FillHeader(hdr, static_cast<uint32_t>(dataSize), sampleRate, channels, bits);
    std::memcpy(buf.data(), &hdr, sizeof(hdr));

    // 16bit 时填 440Hz 正弦 —— 播放/算峰值都有真实波形可看; 其它位深留 0
    if (bits == 16) {
        const double step = 2.0 * 3.14159265358979323846 * 440.0 / sampleRate;
        for (uint32_t i = 0; i < frames; ++i) {
            const int16_t v = static_cast<int16_t>(std::sin(step * i) * 0.5 * 32767.0);
            for (uint16_t c = 0; c < channels; ++c) {
                const size_t off = sizeof(WavHeader) +
                                   (static_cast<size_t>(i) * channels + c) * 2;
                buf[off]     = static_cast<uint8_t>(v & 0xFF);
                buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            }
        }
    }
    return buf;
}

// 录音器的加密开关是"翻转"而不是"置位"(SetAencEncrypt() 不带参数), 且默认是【开】。
// 直接调 SetAencEncrypt() 会在默认值上翻反 —— 先说清要哪个模式, 用这个切。
inline void SetEncrypt(CAudioRecorder& rec, bool on) {
    if (rec.GetAencEncrypt() != on) rec.SetAencEncrypt();
}

// 落盘后的真实文件名: 后缀由加密开关决定, 不是调用方随便定的
inline std::string RecFileName(const std::string& base, const CAudioRecorder& rec) {
    return base + (rec.GetAencEncrypt() ? ".aenc" : ".wav");
}

// 设备打不开时的状态码。这两类是"环境没有", 不是"SDK 错了" → 跳过而非失败。
inline bool DeviceUnavailable(AudioSdk::AudioSdkState st) {
    return st == AudioSdk::AudioSdkState::DEVICE_NOT_FOUND ||
           st == AudioSdk::AudioSdkState::DEVICE_BUSY;
}

}   // namespace tst

#define CHECK(cond, msg)            tst::Report((msg), (cond), __LINE__)
#define CHECK_D(cond, msg, detail)  tst::Report((msg), (cond), __LINE__, (detail))
#define RUN(fn)                     do { std::printf("\n-- %s\n", #fn); fn(); } while (0)
