/* @Created On : 2026/8/12
   @Author : 孟源
   @note : 播放测试脚本（类 API 版）
          用法: test_play.exe [wav文件]
          不带参数时若 tone_test.wav 不存在会自动生成一段 3 秒 440Hz 正弦波。
          测试流程：播放 -> 进度轮询 -> 暂停/恢复 -> Seek -> 自然播完 -> Stop 清理，
          最后再做一次"播放中 Stop 中断"用例。
*/

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "audio_sdk/audio_player.h"
#include "audio_sdk/wav_format.h"      // 生成测试音：CWavFormat::SaveWavFile(默认明文 / bEncrypt=true 加密)

// 把命令行窄字符参数转成宽字符（Windows 下 argv 是系统代码页编码，用 CP_ACP 转）
static bool NarrowToWide(const char* narrow, std::wstring& out)
{
    if (!narrow)
        return false;
    const int len = MultiByteToWideChar(CP_ACP, 0, narrow, -1, nullptr, 0);
    if (len <= 1)
        return false;
    out.resize(static_cast<size_t>(len) - 1);
    MultiByteToWideChar(CP_ACP, 0, narrow, -1, &out[0], len);
    return true;
}

// 生成一段 3 秒 440Hz 正弦波测试音（44.1kHz / 16bit / 单声道），复用 SDK 的落盘接口
static bool GenerateTestTone(const std::wstring& filePath)
{
    const uint32_t kSampleRate = SAMPLE_RATE;
    const double   kFreqHz     = 440.0;
    const double   kSeconds    = 3.0;

    std::vector<int16_t> samples(static_cast<size_t>(kSampleRate * kSeconds));
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * kFreqHz * i / kSampleRate));

    AudioSdk::AudioSdkState st = CWavFormat::SaveWavFile(
        filePath.c_str(), samples.data(), samples.size() * sizeof(int16_t));
    return st == AudioSdk::AudioSdkState::NONE;
}

// 生成 3 秒 440Hz 正弦波并保存为加密 .aenc（验证播放器加密分流）
static bool GenerateEncryptedTone(const std::wstring& filePath)
{
    const uint32_t kSampleRate = SAMPLE_RATE;
    const double   kFreqHz     = 440.0;
    const double   kSeconds    = 3.0;

    std::vector<int16_t> samples(static_cast<size_t>(kSampleRate * kSeconds));
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * kFreqHz * i / kSampleRate));

    // 同一个落盘入口, bEncrypt=true 即存加密容器
    AudioSdk::AudioSdkState st = CWavFormat::SaveWavFile(
        filePath.c_str(), samples.data(), samples.size() * sizeof(int16_t), /*bEncrypt=*/true);
    return st == AudioSdk::AudioSdkState::NONE;
}

// 等待播放结束（带超时保护），期间每 250ms 打印一次进度
static bool WaitFinish(CAudioPlayer& player, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    DWORD lastPrint  = 0;
    while (player.IsPlaying())
    {
        const DWORD now = GetTickCount();
        if (now - start >= timeoutMs)
        {
            std::printf("[error] playback timeout after %lu ms\n",
                        static_cast<unsigned long>(timeoutMs));
            return false;
        }
        if (now - lastPrint >= 250)
        {
            lastPrint = now;
            std::printf("[info] pos %lu / %lu bytes\n",
                        static_cast<unsigned long>(player.GetPlayPos()),
                        static_cast<unsigned long>(player.GetTotalPos()));
        }
        Sleep(50);
    }
    return true;
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 关闭 stdout 缓冲，保证实时看到进度

    std::wstring filePath = L"tone_test.wav";
    if (argc > 1 && !NarrowToWide(argv[1], filePath))
    {
        std::printf("[error] failed to convert file path\n");
        return 1;
    }

    // 测试音不存在就现场生成，保证单跑 exe 也能测
    std::ifstream probe(filePath, std::ios::in | std::ios::binary);
    if (!probe.is_open())
    {
        std::printf("[info] %ls not found, generating test tone ...\n", filePath.c_str());
        if (!GenerateTestTone(filePath))
        {
            std::printf("[error] failed to generate test tone\n");
            return 1;
        }
    }
    else
    {
        probe.close();
    }

    CAudioPlayer player;

    // ---- 用例 1：正常播放 + 暂停/恢复 + Seek + 自然播完 ----
    std::printf("[info] playing %ls ...\n", filePath.c_str());
    AudioSdk::AudioSdkState st = player.PlayWavFile(filePath.c_str());
    if (st != AudioSdk::AudioSdkState::NONE)
    {
        std::printf("[error] PlayWavFile() returned %d\n", static_cast<int>(st));
        return 1;
    }
    std::printf("[info] PlayWavFile() ok, total %lu bytes\n",
                static_cast<unsigned long>(player.GetTotalPos()));

    if (player.GetTotalPos() == 0)
    {
        std::printf("[error] GetTotalPos() is 0, data not loaded\n");
        return 1;
    }

    Sleep(1000);                       // 播 1 秒
    player.Pause();
    if (!player.IsPaused())
    {
        std::printf("[error] Pause() did not take effect\n");
        return 1;
    }
    std::printf("[info] paused, pos %lu\n", static_cast<unsigned long>(player.GetPlayPos()));

    const DWORD posAtPause = player.GetPlayPos();
    Sleep(500);
    if (player.GetPlayPos() > posAtPause && !player.IsPlaying())
    {
        // 暂停期间位置不应大幅前进（缓冲池最多缓存 ~200ms）
        std::printf("[error] position advanced while paused: %lu -> %lu\n",
                    static_cast<unsigned long>(posAtPause),
                    static_cast<unsigned long>(player.GetPlayPos()));
        return 1;
    }

    player.Resume();
    if (player.IsPaused())
    {
        std::printf("[error] Resume() did not take effect\n");
        return 1;
    }
    std::printf("[info] resumed\n");

    Sleep(500);
    const DWORD seekTo = player.GetTotalPos() / 2;   // 跳到中点
    st = player.Seek(seekTo);
    if (st != AudioSdk::AudioSdkState::NONE)
    {
        std::printf("[error] Seek() returned %d\n", static_cast<int>(st));
        return 1;
    }
    std::printf("[info] seek to %lu / %lu\n",
                static_cast<unsigned long>(player.GetPlayPos()),
                static_cast<unsigned long>(player.GetTotalPos()));

    if (!WaitFinish(player, 15000))
        return 1;
    std::printf("[info] playback finished, final pos %lu / %lu\n",
                static_cast<unsigned long>(player.GetPlayPos()),
                static_cast<unsigned long>(player.GetTotalPos()));

    if (player.GetPlayPos() < player.GetTotalPos() / 4)
    {
        std::printf("[error] final position too small, playback did not complete\n");
        return 1;
    }
    player.Stop();                     // 收尾清理
    std::printf("[ok] case 1 (play/pause/resume/seek/finish) passed\n");

    // ---- 用例 2：播放中 Stop 中断 ----
    st = player.PlayWavFile(filePath.c_str());
    if (st != AudioSdk::AudioSdkState::NONE)
    {
        std::printf("[error] second PlayWavFile() returned %d\n", static_cast<int>(st));
        return 1;
    }
    Sleep(700);
    player.Stop();
    if (player.IsPlaying())
    {
        std::printf("[error] Stop() did not take effect\n");
        return 1;
    }
    std::printf("[ok] case 2 (stop while playing) passed\n");

    // ---- 用例 3：播放加密 .aenc（魔数 + 标准 WAV 头 + 密文，播放器应自动解密分流）----
    const std::wstring aencFile = L"tone_test.aenc";
    std::ifstream aencProbe(aencFile, std::ios::in | std::ios::binary);
    if (!aencProbe.is_open())
    {
        std::printf("[info] %ls not found, generating encrypted tone ...\n", aencFile.c_str());
        if (!GenerateEncryptedTone(aencFile))
        {
            std::printf("[error] failed to generate encrypted tone\n");
            return 1;
        }
    }
    else
    {
        aencProbe.close();
    }

    st = player.PlayWavFile(aencFile.c_str());
    if (st != AudioSdk::AudioSdkState::NONE)
    {
        std::printf("[error] PlayWavFile(.aenc) returned %d\n", static_cast<int>(st));
        return 1;
    }
    if (player.GetTotalPos() == 0)
    {
        std::printf("[error] .aenc total pos is 0, decrypt failed\n");
        return 1;
    }
    if (!WaitFinish(player, 15000))
        return 1;
    player.Stop();
    std::printf("[ok] case 3 (play encrypted .aenc) passed\n");

    std::printf("[ok] playback tests ALL passed\n");
    return 0;
}
