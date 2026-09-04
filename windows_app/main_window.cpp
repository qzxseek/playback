#define WIN32_LEAN_AND_MEAN        // 只引入常用 Win32 头, 加快编译
#include <windows.h>

#include "main_window.h"

constexpr wchar_t kClassName[] = L"Audio Recorder & Player";

void CMainWindows::CreateControls(HWND hwnd){
    // 创建按钮
    m_hBtnRec = CreateWindowEx(0, L"BUTTON", L"录制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 10, 100, 30, hwnd, BTN_RECORD, NULL, NULL);
}


LRESULT CALLBACK CMainWindows::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
   switch(msg){
    case WM_CREATE:{
        CreateControls(hwnd);
        return 0;
        break;
    }
    case WM_DESTROY:{                 // 销毁窗口
        PostQuitMessage(0);
        return 0;
        break;
    }
    case WM_COMMAND:{
        const int iId = LOWORD(wParam);
        const int iNotify = HIWORD(wParam);
        if(iNotify == BN_CLICKED){
            // 点击事件
            if(iId == BTN_RECORD){
                // 录制按钮点击
            }
            else if(iId == BTN_STOP){
                // 停止按钮点击
            }
            else if(iId == BTN_PLAY){
                // 播放按钮点击
            }
            else if(iId == BTN_PLAY_PAUSE){
                // 播放/暂停按钮点击
            }
            else if(iId == BTN_ENCRYPT){
                // 加密/取消加密按钮点击
            }
        }
        return 0;
    }
        

    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
    
}
   


int WINAPI CMainWindows::wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine,
    int nCmdShow
){
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    
    if (!RegisterClassW(&wc)) {
        return 0;
    }
    
    HWND hwnd = CreateWindowEx(0,kClassName, L"Win32 音频播放器",
        WS_OVERLAPPEDWINDOW, // 固定大小
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 380,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) return 0;
    
    ShowWindow(hwnd, nCmdShow);          // 显示窗口
    UpdateWindow(hwnd);                  // 更新窗口显示
    
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
