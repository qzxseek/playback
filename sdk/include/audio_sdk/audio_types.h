#pragma once

/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 公共数据类型
*/
#include <vector>
#include <stdio.h>


// 音频参数
#define SAMPLE_RATE     44100
#define BITS_PER_SAMPLE 16
#define CHANNELS        1

// 录音分块时长(毫秒)
//   Windows —— 按它算 winmm 缓冲块的字节数
//   Android —— 按它攒够采样再算一次波形峰值(节奏与 Windows 对齐)
#define AUDIO_SDK_BLOCK_MS           100


#define AUDIO_SDK_WAVE_BLOCK_POINTS  256              // 每块(AUDIO_SDK_BLOCK_MS 的时长)积出这么多个点
#define AUDIO_SDK_WAVE_FILE_POINTS   1024             // 整个文件的波形, 一次给这么多个点

// 录音侧波形的传输容量(见 wave_ring.h 的 CWaveRing)。
// 4096 点 ≈ 1.6 秒 —— 调用方在最坏情况下落后这么久也还能按序取全;
// 真落后更久就覆盖最老的: 丢的是波形, 主录音数据不受影响。
#define AUDIO_SDK_WAVE_RING_POINTS   4096

// 每块点数不能超过环容量, 否则同一次 Push 内部就会自我覆盖
static_assert(AUDIO_SDK_WAVE_BLOCK_POINTS <= AUDIO_SDK_WAVE_RING_POINTS,
              "每块点数不能超过波形环容量 AUDIO_SDK_WAVE_RING_POINTS");

// 波形回调: 把降采样后的峰值对交给调用方画波形
//   minmax: [min0, max0, min1, max1, ...] 已归一化到 [-1, 1], 共 points*2 个 float
//   points: 峰值点个数
//   userData: 注册回调时原样透传的指针
//
// 【目前只有播放器用它】—— AudioSdk_PlayerBuildWaveform 在【调用线程】同步跑完,
// 没有并发: 回调体里想怎么用都行(画界面、调 JNI、分配内存都可以)。
//
// 【录音不在这里】录音波形走的是"拉模式", 不走回调 —— 因为录音的峰值是音频线程
// 算出来的, 而这个回调类型不携带任何线程信息, 一旦有调用方把自己的代码挂到音频
// 线程上, "禁止分配内存/加锁/碰界面"这条就只能靠注释自觉, 类型系统拦不住。
// 所以 SDK 干脆不给那个入口: 峰值由 CWaveRing 攒在 SDK 内部, 调用方在任意线程
// 用 AudioSdk_RecorderReadWave 取走。
typedef void (*AudioSdkWaveCallback)(const float* minmax, int points, void* userData);

// SDK 接口状态
namespace AudioSdk{
enum class AudioSdkState
{
    NONE,                  // 无错误
    DEVICE_NOT_FOUND,      // 设备无法打开
    DEVICE_BUSY,           // 设备已被占用
    FORMAT_NOT_SUPPORTED,  // 格式不支持
    FILE_OPEN_FAILED,      // 文件打开失败
    FILE_WRITE_FAILED,     // 文件写入失败
    FILE_READ_FAILED,      // 文件读取失败
    INVALID_PARAMETER,     // 无效参数
    OUT_OF_MEMORY,         // 内存不足
    PLATFORM_ERROR,        // 平台错误
    UNKNOWN_ERROR,         // 未知错误
};
}