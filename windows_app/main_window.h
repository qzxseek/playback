/* @Created On : 2026/9/4
   @Author : 孟源
   @note : Win32 音频 UI 主窗口(类封装: 主窗口 + 控件句柄 + 消息处理)
*/
#pragma once                       // 防止头文件被重复包含

// 必须先定义 UNICODE 再包含 windows.h, 否则 CreateWindowEx 等会展开成
// 窄字符版 CreateWindowExA, 与宽字符串 L"..." 冲突 → C2664(你第 10 行正是这个)。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>       
#include <string>

#include "audio_sdk/audio_player.h"
#include "audio_sdk/audio_recorder.h"

#define BTN_RECORD          1001      // 录制按钮
#define BTN_RECORD_PAUSE    1002      // 录制/暂停按钮
#define BTN_STOP            1003      // 停止录制按钮
#define BTN_PLAY            1004      // 播放按钮
#define BTN_PLAY_PAUSE      1005      // 播放/暂停按钮
#define BTN_STOP_PLAY       1006      // 停止播放按钮
#define BTN_ENCRYPT         1007      // 加密/取消加密按钮
#define BTN_OPEN_FILE       1008      // 打开文件按钮

#define WM_WAVEIN_DONE (WM_USER + 1)

class CMainWindows
{
public:
    CMainWindows();              
    ~CMainWindows();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 实例方法: 真正的消息处理(静态 WndProc 转发到这里), 可访问成员/控件句柄。
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 记录主窗口句柄 + 创建全部控件。WM_CREATE 时由 HandleMessage 调用。
    void OnCreate(HWND hwnd);

    HWND GetHwnd() const { return m_hwnd; }    // 供 main.cpp / 外部访问主窗口句柄

    // 音频录制相关
    void AudioStartRec(CAudioRecorder cRecorder);
    void AudioPauseResumeRec(CAudioRecorder cRecorder);
    void AudioStopRec(CAudioRecorder cRecorder);

    // 音频播放相关
    void AudioStartPlay(CAudioPlayer cPlayer);
    void AudioPauseResumePlay(CAudioPlayer cPlayer);
    void AudioStopPlay(CAudioPlayer cPlayer);

    // 打开文件对话框, 选中音频文件后把路径存进 m_curFile 并返回 true
    bool OpenFileDialog(HWND hwndOwner);
private:
    void CreateControls(HWND hwnd);           // 在父窗口里创建全部控件
    void OnCommand(int iId);                  // WM_COMMAND 按钮分发(点击逻辑入口)

    bool m_isRecording = false;                     // 是否正在录制
    std::wstring m_curFile;                         // 当前选中待播放的音频文件路径
    HWND m_hwnd                = NULL;              // 主窗口句柄
    HWND m_hBtnRec_Start_Stop  = NULL;              // 录制/停止
    HWND m_hBtnRecPause        = NULL;              // 录制继续/暂停
    HWND m_hBtnPlay_Start_Stop = NULL;              // 播放/停止
    HWND m_hBtnPlayPause       = NULL;              // 继续播放/暂停
    HWND m_hChkEnc             = NULL;              // 加密复选框
    HWND m_hBtnOpen            = NULL;              // 打开文件按钮
};