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

#if (_WIN32)
#define BUFFER_COUNT    4
#define BUFFER_SIZE     (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE/8) / 10) // 100ms

#include "audio_sdk/audio_export.h"

#include <windows.h>
#endif


#define AUDIO_SDK_WAVE_BLOCK_POINTS  256              // 每块(100ms)回调一次, 每次给这么多个点
#define AUDIO_SDK_WAVE_FILE_POINTS   1024             // 整个文件的波形, 一次给这么多个点

// 波形回调: 把降采样后的峰值对交给调用方画波形
//   minmax: [min0, max0, min1, max1, ...] 已归一化到 [-1, 1], 共 points*2 个 float
//   points: 峰值点个数
//   userData: 注册回调时原样透传的指针
//
// 录音时本回调跑在【音频线程】上, 只允许做"拷贝数据 + PostMessage"这类极快的事;
// 禁止分配内存、加锁、调用 GDI/窗口 API、做耗时计算 —— 慢了会丢音频块。
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