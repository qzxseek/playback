#pragma once

/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 公共数据类型
*/
#include <vector>
#include <windows.h>
#include <stdio.h>


// 音频参数
#define SAMPLE_RATE     44100
#define BITS_PER_SAMPLE 16
#define CHANNELS        1
#define BUFFER_COUNT    4
#define BUFFER_SIZE     (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE/8) / 10) // 100ms

#define BTN_RECORD   1001      // 录制按钮
#define BTN_PAUSE    1002      // 暂停按钮
#define BTN_RESUME   1003      // 恢复按钮
#define BTN_STOP     1004      // 停止按钮
#define BTN_PLAY     1005      // 播放按钮
#define WM_WAVEIN_DONE (WM_USER + 1)

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