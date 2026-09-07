/* @Created On : 2026/9/4
   @Author : 孟源
   @note : Win32 音频 UI 主窗口(类封装: 主窗口 + 控件句柄 + 消息处理)
   设计要点:
   - 程序入口 wWinMain 放 main.cpp(全局函数), 本类只管"窗口 + 控件 + 消息"。
   - 窗口过程 WndProc 必须是【静态】成员(系统回调签名不能带 this),
     它通过全局指针 g_pMain 转发到实例的 HandleMessage(见 main.cpp 顶部)。
   - CAudioPlayer / CAudioRecorder 是【本类成员】(生命周期与窗口一致)——
     这样播放状态才能跨按钮点击保持, 进度条才能实时轮询/拖拽跳转。
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
#include <commdlg.h>       // GetOpenFileNameW(打开文件对话框)
#include <string>

#include "audio_sdk/audio_player.h"
#include "audio_sdk/audio_recorder.h"

// ---------------- 控件 / 消息 ID ----------------
#define BTN_RECORD_START_STOP          1001   // 开始录音 / 停止录音(同一个按钮切换文字)
#define BTN_RECORD_PAUSE    1002   // 暂停/继续录音
#define BTN_STOP            1003   // (保留) 停止录音
#define BTN_START_STOP_PLAY 1004   // 开始播放 / 停止播放(按当前状态)
#define BTN_PLAY_PAUSE      1005   // 暂停/继续播放
#define BTN_ENCRYPT         1006   // 加密复选框
#define BTN_OPEN_FILE       1007   // 打开文件按钮
#define IDC_LBL_TIME        1008   // 时间/状态文字
#define IDC_LBL_REC_TIME    1009   // 录音时间文字  

#define WM_WAVEIN_DONE (WM_USER + 1)

// 进度条刷新定时器 ID 与间隔
#define TIMER_PROGRESS      1
#define TIMER_INTERVAL_MS   100     // 每 100ms 问一次播放器播到哪

class CMainWindows
{
public:
    CMainWindows();
    ~CMainWindows();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void OnCreate(HWND hwnd);
    HWND GetHwnd() const { return m_hwnd; }

private:
    void CreateControls(HWND hwnd);
    void OnCommand(int iId);

    // ---------- 录音 ----------
    void AudioStartRec();              // 开始 / 停止(按当前状态)
    void AudioPauseResumeRec();
    void AudioStopRec();
    void UpdateRecTimeUI(DWORD sec);

    // ---------- 播放 ----------
    void AudioStartStopPlay();
    void AudioPauseResumePlay();
    bool OpenFileDialog(HWND hwndOwner);

    // ---------- 进度条 ----------
    void OnTimerTick();               // 每 100ms: 刷新进度条 & 状态文字; 自然播完复位
    void UpdateProgressUI();          // 按 m_playPosBytes / m_playTotalBytes 画条 + 刷文字
    void InvalidateProgress();        // 让进度条区域重绘
    void DrawProgress(HDC hdc);       // 实际绘制(轨道 + 已播填充)
    RECT ProgressRect() const;        // 进度条所在客户区矩形(与控件布局对应)
    DWORD ClampToBytes(int clientX);  // 客户区 x → 对应字节位置(夹到 [0,total])

    // ---------- 成员 ----------
    bool m_isRecording = false;       // 是否正在录音(含暂停)
    bool m_recPaused   = false;
    bool m_isPlaying   = false;       // 是否正在播放(含暂停)
    bool m_playPaused  = false;
    bool m_dragging    = false;       // 用户是否正按住进度条拖动

    CAudioRecorder m_recorder;        // 长命成员: 录音
    CAudioPlayer   m_player;          // 长命成员: 播放

    std::wstring m_curFile;           // 当前打开的文件(播放用)

    DWORD m_playPosBytes  = 0;        // 当前播放位置(字节), 定时器刷新
    DWORD m_playTotalBytes = 0;       // 当前播放总长(字节)

    HWND m_hwnd                = NULL;
    HWND m_hBtnRec_Start_Stop  = NULL;   // 开始/停止录音
    HWND m_hBtnRecPause        = NULL;   // 暂停/继续录音
    HWND m_hChkEnc             = NULL;   // 加密复选框
    HWND m_hBtnOpen            = NULL;   // 打开文件
    HWND m_hBtnPlay_Start_Stop = NULL;   // 播放/停止播放按当前状态
    HWND m_hBtnPlayPause       = NULL;   // 暂停/继续播放
    HWND m_hBtnPlayStop        = NULL;   // 停止播放
    HWND m_hLblRecTime         = NULL;   // 录音时间文字
    HWND m_hLblTime            = NULL;   // 时间/状态文字
};