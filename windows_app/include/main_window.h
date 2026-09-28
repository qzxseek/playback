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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// 波形不再用自定义窗口消息: 改拉模式后, 音频线程不会主动通知 UI,
// 由 TIMER_PROGRESS 定时器每 100ms 去 SDK 拉一次(见 ConsumeWaveRing)。

// 进度条刷新定时器 ID 与间隔
#define TIMER_PROGRESS      1
#define TIMER_INTERVAL_MS   100     // 每 100ms 问一次播放器播到哪, 顺便拉一次录音波形

#define WM_APP_SDK_DONE     (WM_APP + 1)

// ---------------- 波形参数 ----------------
// 录音侧每 100ms 积出 256 个点(AUDIO_SDK_WAVE_BLOCK_POINTS), 即每点约 0.39ms。
// 滚动窗口 2048 点 ≈ 0.8 秒, 是个看得清楚的"示波器"尺度。
// 绘制时若点数多于像素列数, 会按列再合并一次(取该列范围的 min/max)。
//
// 这只是 UI 侧的显示窗口, 与 SDK 侧的传输缓冲(那里的容量是
// AUDIO_SDK_WAVE_RING_POINTS)无关: 波形由 SDK 攒在环里、UI 每 100ms 拉一次,
// 拉回多少就往这个窗口里接多少。容量关系因此不构成编译期不变量, 故作罢。
#define REC_WAVE_POINTS     2048

// ---------------- 双链路工作线程 ----------------
namespace Cmd {
// 命令码
enum Op {
    RecStart = 1,   
    RecStop  = 2,   
    PlayFile = 3,   
    Exit     = 99,  
};

struct RecCommand { Op op; void* handle = nullptr; };
// 播放命令
struct PlayCommand { Op op; std::wstring path; };   

template <typename T>
class Queue {
public:
    void Push(T cmd){
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_cmds.push_back(std::move(cmd));
        }
        m_cv.notify_one();
    }
    // 阻塞取一条; 收到 Exit 命令返回 false(工作线程据此退出)
    bool Pop(T& out){
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait(lk, [this]{ return !m_cmds.empty(); });
        out = std::move(m_cmds.front());
        m_cmds.pop_front();
        return out.op != Exit;
    }
private:
    std::mutex              m_mtx;
    std::condition_variable m_cv;
    std::deque<T>           m_cmds;
};

}   // namespace Cmd

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

    // ---------- 双链路工作线程 ----------
    void StartWorkers();                        // WM_CREATE: 起两条录音 lane + 播放线程
    void StopWorkers();                         // 析构/收尾: 投 Exit → join 全部线程
    static void RecWorkerLoop(CMainWindows* self, int lane);  // 录音 lane 主循环(静态入口 + this)
    static void PlayWorkerLoop(CMainWindows* self);           // 播放链主循环
    void OnSdkDone(UINT op, UINT lane, int state);  // WM_APP_SDK_DONE: 按命令码分岔收结果

    // ---------- 录音 ----------
    void AudioStartStopRec();              // 开始 / 停止(按当前状态)
    void AudioPauseResumeRec();
    void UpdateRecTimeUI(DWORD ms);         // 传毫秒, 内部格式化成 mm:ss.d
    void* TakeRecorder(std::wstring& outPath);  // 新建对象存入容器
    std::wstring NextOutputPath();          // 每会话输出路径
    void SetRecGroupEnabled(bool on);       // 录音组按钮统一置灰/恢复

    // ---------- 播放 ----------
    void AudioStartStopPlay();
    void AudioPauseResumePlay();
    bool OpenFileDialog(HWND hwndOwner);
    void ShowPlayError(int state);          // 按状态码给播放失败文案(开始/停止共用)
    void SetPlayGroupEnabled(bool on);      // 播放组按钮统一置灰/恢复(投命令时用)
    void SetPlayGroupPlaying();             // 播放中布局(停止/暂停可用, 录音组禁用)
    void SetPlayGroupIdle();                // 非播放中布局(尊重录音状态与忙标志)

    // ---------- 进度条 ----------
    void OnTimerTick();               // 每 100ms: 刷新进度条 & 状态文字; 自然播完复位
    void UpdateProgressUI();          // 按 m_playPosBytes / m_playTotalBytes 画条 + 刷文字
    void InvalidateProgress();        // 让进度条区域重绘
    void DrawProgress(HDC hdc);       // 实际绘制(轨道 + 已播填充)
    RECT ProgressRect() const;        // 进度条所在客户区矩形(与控件布局对应)
    DWORD ClampToBytes(int clientX);  // 客户区 x → 对应字节位置(夹到 [0,total])

    // ---------- 波形 ----------
    // 录音波形没有回调: SDK 把峰值写进它自己的环形缓冲, 在 UI 线程拉
    static void OnWaveFromFile (const float* minmax, int points, void* userData);

    bool ConsumeWaveRing();           // UI 线程: 从 SDK 拉波形点并进滚动窗口; 返回是否取到
    void AppendToRecWave(const float* minmax, int points);   // 把一批点接到滚动窗口末尾
    void InvalidateWave();            // 让波形区重绘
    void DrawWaveform(HDC hdc);       // 实际绘制(中轴线 + 波形 + 播放位置)
    RECT WaveRect() const;            // 波形区客户区矩形
    // 文件波形的跨线程搬运(播放线程写 / UI 读, 见成员区注释)
    void CopyFileWave(const float* minmax, int points);      // 播放线程: 锁内写一整批
    int  SnapshotFileWave(float (&out)[AUDIO_SDK_WAVE_FILE_POINTS][2]) const;  // UI: 锁内拷出

    // ---------- 成员 ----------
    bool m_isRecording = false;       // 是否正在录音(含暂停)
    bool m_recPaused   = false;
    bool m_isPlaying   = false;       // 是否正在播放(含暂停)
    bool m_playPaused  = false;
    bool m_dragging    = false;       // 用户是否正按住进度条拖动

    bool m_recBusy = false;           // 录音设备忙碌判断
    bool m_playBusy = false;          // 播放设备忙碌判断
   
    // 每 lane 当前会话的录音器句柄(Start 时记, Stop 投出时取走)
    void* m_laneRec[2] = { nullptr, nullptr };
    int   m_activeLane = 0;           // 正在进行的会话用的 lane 号
 
    void* m_savingHandle[2] = { nullptr, nullptr };
    std::wstring m_lanePath[2];       // 各 lane 当前会话的输出路径(保存完成提示用)

    unsigned m_outputSeq = 0;         // 输出文件名序号(单调递增, 保证同秒内也不重名)

    bool m_encryptOn = true;
    
    AudioRecSdkApi m_recApi;         
    AudioPlaSdkApi m_playApi;        

    void* m_recorderHandle = nullptr;     // 正在录音的录音器对象
    void* m_playerHandle   = nullptr;     // 播放器对象
    std::vector<void*> m_recorderHandles; // 录音器句柄列表(乒乓交替 + 批量销毁)
    std::vector<void*> m_playerHandles;   // 播放器句柄列表(用于批量销毁)

    // ---------- 双链路工作线程 ----------
    struct RecLane {
        Cmd::Queue<Cmd::RecCommand> q;      // 该 lane 的命令队列
        std::thread worker; // 该 lane 的工作线程
    };
    RecLane m_recLanes[2];
    int     m_recLaneIdx = 0;               // 下一次录音用哪条 lane(乒乓翻转)

    Cmd::Queue<Cmd::PlayCommand> m_playQ;   // 播放命令队列
    std::thread m_playWorker;               // 播放链工作线程(执行 PlayFile+BuildWaveform)

    
    std::wstring m_curFile;           // 当前打开的文件(播放用)

    DWORD m_playPosBytes  = 0;        // 当前播放位置(字节), 定时器刷新
    DWORD m_playTotalBytes = 0;       // 当前播放总长(字节)

    // ---------- 波形数据 ----------
    // 录音波形: 环形缓冲在 SDK 那边, 这里只是"每次从 SDK 拉一批"的中转缓冲。
    float m_wavePull[AUDIO_SDK_WAVE_BLOCK_POINTS * 2] = {};

    float m_recWave[REC_WAVE_POINTS][2] = {};                        // 录音波形数据
    int   m_recWaveCount = 0;                                      // 录音有效点

    mutable std::mutex m_fileWaveMtx;  
    float m_fileWave[AUDIO_SDK_WAVE_FILE_POINTS][2] = {};            // 文件波形数据
    int   m_fileWaveCount = 0;                                      // 文件有效点
    float m_fileWaveSnap[AUDIO_SDK_WAVE_FILE_POINTS][2] = {};        // UI 绘制用的快照(仅 UI 线程碰)

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