/* @Created On : 2026/8/12
   @Author : 孟源
   @note : 录音测试脚本（类 API 版）
          用法: test_record.exe [录制总时长秒数]   （默认 3 秒）
          SDK 录制走 CALLBACK_FUNCTION，回调在 winmm 线程上，无需窗口/消息循环。
          时间线：录 1s -> 暂停 1s -> 继续录 (总时长-2)s -> 停止（SDK 内部落盘 output.wav）。
          校验：output.wav 存在、时长 ≈ 总时长-1s（暂停的 1 秒不应被录进去）。
*/

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "audio_sdk/audio_types.h"     // SAMPLE_RATE 等宏 + AudioSdkState
#include "audio_sdk/audio_recorder.h"  // CAudioRecorder
#include "audio_sdk/encrypted_format.h" // IsAencFile / kAencPrefixSize（录音落盘为 .aenc）
#include "audio_sdk/wav_format.h"      // WavHeader（校验落盘文件）

// 读文件大小；不存在返回 0
static unsigned long long GetFileSize(const wchar_t* filePath)
{
    std::ifstream file(filePath, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return 0;
    return static_cast<unsigned long long>(file.tellg());
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 关闭 stdout 缓冲，保证实时看到进度

    // 总时长 = 录 5s + 暂停 1s + 录 (总时长-2)s，最少 5 秒
    DWORD totalSec = 5;
    if (argc > 1)
    {
        int secs = std::atoi(argv[1]);
        if (secs >= 3)
            totalSec = static_cast<DWORD>(secs);
        else
            std::printf("[warn] duration < 3s, using default 3s\n");
    }

    CAudioRecorder recorder;

    // ---- 开始录制 ----
    AudioSdk::AudioSdkState st = recorder.StartRecording();
    if (st != AudioSdk::AudioSdkState::NONE)
    {
        std::printf("[error] StartRecording() returned %d\n", static_cast<int>(st));
        return 1;
    }
    std::printf("[info] recording %lu s (with a 1 s pause in the middle) ...\n",
                static_cast<unsigned long>(totalSec));

    Sleep(5000);                       // 正常录 5 秒

    // ---- 暂停 ----
    recorder.PauseResumeRecording();
    Sleep(1000);                       // 暂停 1 秒：这段不应进数据
    std::printf("[info] paused 1s\n");

    // ---- 继续录制到总时长 ----
    recorder.PauseResumeRecording();
    Sleep((totalSec - 2) * 1000);
    std::printf("[info] resumed and recorded to end\n");

    // ---- 停止（SDK 内部保存 output.wav）----
    st = recorder.StopRecording();
    if (st != AudioSdk::AudioSdkState::NONE)
    {
        std::printf("[error] StopRecording() returned %d\n", static_cast<int>(st));
        return 1;
    }

    // ---- 校验落盘文件（应是加密容器 .aenc）----
    const wchar_t* outFile = L"output.aenc";
    const unsigned long long size = GetFileSize(outFile);
    if (size < CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader))
    {
        std::printf("[error] %ls missing or too small (%llu bytes)\n", outFile, size);
        return 1;
    }

    // 读回整个文件：判魔数，剥 6 字节前缀后取内部标准 WAV 头的 data 区大小
    std::ifstream file(outFile, std::ios::in | std::ios::binary);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    file.close();

    if (!CEncryptedFormat::IsAencFile(buf.data(), buf.size()))
    {
        std::printf("[error] %ls is not an .aenc container (magic missing)\n", outFile);
        return 1;
    }
    WavHeader hdr = {};
    std::memcpy(&hdr, buf.data() + CEncryptedFormat::kAencPrefixSize, sizeof(hdr));

    const size_t bytesPerSec = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
    const size_t pcmBytes    = hdr.dataSize;
    const size_t recordedMs  = bytesPerSec ? pcmBytes * 1000 / bytesPerSec : 0;

    std::printf("[info] saved %llu bytes (prefix+WAV header %llu + data %zu bytes, ~%zu ms)\n",
                size,
                static_cast<unsigned long long>(CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader)),
                pcmBytes, recordedMs);

    // 期望：总时长 - 暂停的 1 秒，给 1 秒的计时误差余量
    const size_t expectMs   = (totalSec - 1) * 1000;
    const size_t tolerance  = 1000;
    if (recordedMs + tolerance < expectMs)
    {
        std::printf("[error] recorded %zu ms, expected ~%zu ms (data lost?)\n",
                    recordedMs, expectMs);
        return 1;
    }
    if (recordedMs > expectMs + tolerance)
    {
        std::printf("[error] recorded %zu ms, expected ~%zu ms (pause not applied?)\n",
                    recordedMs, expectMs);
        return 1;
    }
    if (pcmBytes < BUFFER_SIZE)   // 至少 100ms，才算真正采到了声音
    {
        std::printf("[error] too little data captured, check mic\n");
        return 1;
    }

    std::printf("[ok] recording pipeline OK (start/pause/resume/stop/save)\n");
    return 0;
}
