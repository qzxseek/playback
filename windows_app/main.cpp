/* @Created On : 2026/9/4
   @Author : 孟源
   @note : Win32 音频 UI 程序入口(WinMain + 消息循环)
   窗口/控件/消息处理逻辑都在 CMainWindows(见 main_window.h/.cpp)。
   本文件只做入口该做的事:
   1) 定义全局 g_pMain(供静态 WndProc 转发回实例)
   2) 注册窗口类(窗口过程 = CMainWindows::WndProc)
   3) 创建主窗口
   4) 进入消息循环
*/
#define WIN32_LEAN_AND_MEAN

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <mmeapi.h>          
#include "main_window.h"

// 全局实例指针: main_window.cpp 的静态 WndProc 靠它转发消息到实例方法
CMainWindows* g_pMain = nullptr;

// 主窗口类名(注册与创建必须一致)
constexpr wchar_t kClassName[] = L"AudioRecPlayMainWnd";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow){
    
    CMainWindows mainWindow;
    g_pMain = &mainWindow;                     // WndProc 现在能把消息转给它

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = CMainWindows::WndProc;  // 静态成员, 可作窗口过程
    wc.hInstance     = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc))
        return 0;

    // 创建主窗口(触发 WM_CREATE → mainWindow 建控件)
    HWND hwnd = CreateWindowExW(
        0, kClassName, L"Win32 音频播放器",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 220,
        NULL, NULL, hInstance, NULL);
    if (!hwnd)
        return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
