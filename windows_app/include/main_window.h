/* @Created On : 2026/9/4
   @Author : 孟源
   @note : Win32 音频 UI 主窗口(类封装: 主窗口 + 控件句柄 + 消息处理)
*/
#pragma once                       

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>
#include <atomic>
#include <string>

// 显式加载器 AudioSdkApi —— 本程序接 SDK 的入口(它自己会带上函数声明那张契约表)。
// 它属于接入层, 和 sdk/ 平级放在 sdk_loader/; 路径由 CMake 的 audio_sdk_loader 目标带进来。
#include "audio_sdk_loader.h"

// ---------------- 控件 / 消息 ID ----------------
#define BTN_RECORD_START_STOP          1001   // 开始录音 / 停止录音(同一个按钮切换文字)
#define BTN_RECORD_PAUSE    1002   // 暂停/继续录音
#define BTN_START_STOP_PLAY 1003   // 开始播放 / 停止播放(按当前状态)
#define BTN_PLAY_PAUSE      1004   // 暂停/继续播放
#define BTN_ENCRYPT         1005   // 加密复选框
#define BTN_OPEN_FILE       1006   // 打开文件按钮
#define IDC_LBL_TIME        1007   // 时间/状态文字
#define IDC_LBL_REC_TIME    1008   // 录音时间文字  

#define WM_WAVEIN_DONE (WM_USER + 1)
#define WM_WAVE_DATA   (WM_USER + 2)   // 音频线程推来一批峰值(见 OnWaveFromAudio)

// 进度条刷新定时器 ID 与间隔
#define TIMER_PROGRESS      1
#define TIMER_INTERVAL_MS   100     // 每 100ms 问一次播放器播到哪

// ---------------- 波形参数 ----------------
// 录音侧每 100ms 推 256 个点(CWaveform::kPointsPerBlock), 即每点约 0.39ms。
// 滚动窗口 2048 点 ≈ 0.8 秒, 是个看得清楚的"示波器"尺度。
// 绘制时若点数多于像素列数, 会按列再合并一次(取该列范围的 min/max)。
#define REC_WAVE_POINTS     2048
// 环形缓冲容量: 音频线程写、UI 线程读的单生产者单消费者队列。
// 比滚动窗口大一倍(≈1.6 秒), 保证 UI 偶尔晚一拍也不会被生产者追尾。
#define WAVE_RING_POINTS    4096

// 下面两条是这几个常量之间必须成立的关系, 写成编译期断言 —— 以后谁调整了
// 数值而破坏了关系, 编译就会当场报错, 而不是运行期出怪现象。
//   环形缓冲必须装得下一次推来的点数, 否则同一个回调内部就会自我覆盖
//      (见 OnWaveFromAudio 的防御分支)
static_assert(AUDIO_SDK_WAVE_BLOCK_POINTS <= WAVE_RING_POINTS,
              "WAVE_RING_POINTS 必须 >= 每块点数 AUDIO_SDK_WAVE_BLOCK_POINTS");
//   滚动窗口不能比环形还大, 否则窗口永远填不满(见 ConsumeWaveRing)
static_assert(REC_WAVE_POINTS <= WAVE_RING_POINTS,
              "REC_WAVE_POINTS 不能大于 WAVE_RING_POINTS");

// ---------------- 布局 ----------------
namespace Layout {

// 期望的客户区尺寸
constexpr int kClientWidth = 584;

constexpr int kMargin    = 10;   // 左/上外边距(所有区域的公共起点)
constexpr int kGap       = 6;    // 同一行控件之间的水平间隔
constexpr int kColGap    = 4;    // 按钮列与右侧文字列之间的间隔(比按钮之间紧一点)
constexpr int kRowHeight = 26;   // 按钮/复选框的行高
constexpr int kBlockGap  = 14;   // 行块之间的垂直间隔

// 所有区域的公共右边界: 右外边距与左边距对称, 因此绝不会越出客户区
constexpr int kRight = kClientWidth - kMargin;

// ---- 三行(从上到下) ----
constexpr int kRecRowTop  = kMargin;                              // 录音行
constexpr int kPlayRowTop = kRecRowTop  + kRowHeight + kGap;      // 播放行
constexpr int kProgTop    = kPlayRowTop + kRowHeight + kBlockGap; // 进度条
constexpr int kProgHeight = 16;
constexpr int kWaveTop    = kProgTop + kProgHeight + kBlockGap;   // 波形区
constexpr int kWaveHeight = 250;
constexpr int kBottomPad  = 30;   // 波形底边到客户区底边的留白

constexpr int kClientHeight = kWaveTop + kWaveHeight + kBottomPad;

// ---- 录音行控件的 x(依次排开) ----
constexpr int kBtnW      = 100;
constexpr int kSmallBtnW = 80;
constexpr int kRecBtnX   = kMargin;
constexpr int kRecPauseX = kRecBtnX   + kBtnW + kGap;
constexpr int kEncX      = kRecPauseX + kBtnW + kGap;

// ---- 播放行控件的 x ----
constexpr int kOpenX      = kMargin;
constexpr int kPlayBtnX   = kOpenX    + kBtnW      + kGap;
constexpr int kPlayPauseW = 90;
constexpr int kPlayPauseX = kPlayBtnX + kSmallBtnW + kGap;

// ---- 右列文字: 从按钮列右边让开一点, 一直铺到公共右边界 ----
constexpr int kTextColX = kEncX + kSmallBtnW + kColGap;
constexpr int kTextColW = kRight - kTextColX;

// 文字比同行按钮矮一截, 在行内垂直居中
constexpr int kLabelH     = 18;
constexpr int kLabelInset = (kRowHeight - kLabelH) / 2;

}   // namespace Layout

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

    // dll 是否加载成功(失败时音频按钮会被禁掉, 这里再兜一道底, 免得调空函数指针)
    bool SdkReady() const;

    // ---------- 录音 ----------
    void AudioStartStopRec();              // 开始 / 停止(按当前状态)
    void AudioPauseResumeRec();
    void UpdateRecTimeUI(DWORD ms);         // 传毫秒, 内部格式化成 mm:ss.d

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

    // ---------- 波形 ----------
    // 这两个是回调解调用的静态函数: 成员函数签名对不上, 用 userData 把 this 传进来
    static void OnWaveFromAudio(const float* minmax, int points, void* userData);  // 音频线程
    static void OnWaveFromFile (const float* minmax, int points, void* userData);  // 调用线程

    void ConsumeWaveRing();           // UI 线程: 把环形缓冲里的新点并进滚动窗口
    void InvalidateWave();            // 让波形区重绘
    void DrawWaveform(HDC hdc);       // 实际绘制(中轴线 + 波形 + 播放位置)
    RECT WaveRect() const;            // 波形区客户区矩形

    // ---------- 成员 ----------
    bool m_isRecording = false;       // 是否正在录音(含暂停)
    bool m_recPaused   = false;
    bool m_isPlaying   = false;       // 是否正在播放(含暂停)
    bool m_playPaused  = false;
    bool m_dragging    = false;       // 用户是否正按住进度条拖动

    // 显式加载: 不用 C++ 类对象(GetProcAddress 取不到"类"), 改成"函数表 + 句柄"
    AudioSdkApi m_api;                    // 显式加载器, 里有全部函数指针(见 audio_sdk.h)
    void* m_recorderHandle = nullptr;     // 录音器对象, 由 dll 的 RecorderCreate 创建
    void* m_playerHandle   = nullptr;     // 播放器对象, 由 dll 的 PlayerCreate 创建

    std::wstring m_curFile;           // 当前打开的文件(播放用)

    DWORD m_playPosBytes  = 0;        // 当前播放位置(字节), 定时器刷新
    DWORD m_playTotalBytes = 0;       // 当前播放总长(字节)

    // ---------- 波形数据 ----------
    // 环形缓冲: 音频线程写(生产), UI 线程读(消费)。单生产者单消费者, 无锁。
    // 音频线程只动 m_waveWritePos, UI 线程只动 m_waveReadPos, 靠 acquire/release 同步。
    float m_waveRing[WAVE_RING_POINTS][2] = {};   // [i][0]=min, [i][1]=max
    std::atomic<int> m_waveWritePos{0};           // 写游标，音频线程写
    int   m_waveReadPos = 0;                      // 读游标，UI 线程读

    float m_recWave[REC_WAVE_POINTS][2] = {};                        // 录音波形数据
    int   m_recWaveCount = 0;                                      // 录音有效点

    float m_fileWave[AUDIO_SDK_WAVE_FILE_POINTS][2] = {};            // 文件波形数据
    int   m_fileWaveCount = 0;                                      // 文件有效点

    HWND m_hwnd                = NULL;
    HWND m_hBtnRec_Start_Stop  = NULL;   // 开始/停止录音
    HWND m_hBtnRecPause        = NULL;   // 暂停/继续录音
    HWND m_hChkEnc             = NULL;   // 加密复选框
    HWND m_hBtnOpen            = NULL;   // 打开文件
    HWND m_hBtnPlay_Start_Stop = NULL;   // 播放/停止播放按当前状态
    HWND m_hBtnPlayPause       = NULL;   // 暂停/继续播放
    HWND m_hLblRecTime         = NULL;   // 录音时间文字
    HWND m_hLblTime            = NULL;   // 时间/状态文字
};