/* @Created On : 2026/9/17
   @Author : 孟源
   @note : 崩溃转储实现 —— 程序崩了用 MiniDumpWriteDump 写出 .dmp。
*/
#define WIN32_LEAN_AND_MEAN

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "include/crash_dump.h"

#include <windows.h>
#include <dbghelp.h>        // MiniDumpWriteDump
#include <exception>        // std::set_terminate

#pragma comment(lib, "dbghelp.lib")     // 直接用系统的 dbghelp, 不动态加载(见文件顶说明)

namespace {

wchar_t g_dir[MAX_PATH] = {};                             // 转储目录(安装时定好, 崩溃时直接用)
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;      // 挂之前是谁, 卸载时还给它

/**
 * @brief 把崩溃现场存成 .dmp
 * @param info 现场(哪个地址、什么错)。传 NULL 也能写, 只是少了这两样
 * @note 只用栈上定长缓冲, 不分配内存 —— 见文件顶的两条纪律
 */
void WriteDump(EXCEPTION_POINTERS* info) {
    if (g_dir[0] == L'\0') return;                  // 还没安装, 没地方写

    SYSTEMTIME t = {};
    ::GetLocalTime(&t);                             // 不分配内存

    wchar_t path[MAX_PATH + 64] = {};
    ::wsprintfW(path, L"%s\\crash_%04d%02d%02d_%02d%02d%02d.dmp",
                g_dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    HANDLE f = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;          // 写不了就退出

    MINIDUMP_EXCEPTION_INFORMATION me = {};
    me.ThreadId = ::GetCurrentThreadId();           // 崩的是哪个线程
    me.ExceptionPointers = info;                    // 现场就在这个指针里
    me.ClientPointers = FALSE;                      // 同进程, 指针可以直接用

    // MiniDumpNormal = 只抓线程栈和相关内存, 文件几十 KB, 够定位问题
    // 想看"所有变量的值"可以换成 MiniDumpWithFullMemory, 但文件会涨到几百 MB
    ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), f,
                        MiniDumpNormal, info ? &me : nullptr, nullptr, nullptr);
    ::CloseHandle(f);
}

/**
 * @brief 处理不了的异常
 * @note 管两类:
 *         空指针 / 除零 / 栈溢出这类 SEH
 *         "throw 出去没人接"的 C++ 异常 —— MSVC 会先把它转成一个 SEH 异常
 *           (码 0xE06D7363), 所以也会走到这里
 */
LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
    WriteDump(info);
    return EXCEPTION_EXECUTE_HANDLER;   // "我处理完了", 系统接着结束进程
}

/**
 * @brief 入口二: terminate
 * @note 管的是【不走 OnCrash】的那批: 显式 std::terminate()、noexcept 里抛异常、
 *       析构函数里抛、异常传播过程中又抛。这些没有"现场"可拿(info 只能传 NULL),
 *       但各线程的栈还在, 定位够用了
 */
void OnTerminate() {
    WriteDump(nullptr);
    ::ExitProcess(3);                   // 别让它再绕回 abort()
}

}   // namespace

void InstallCrashHandler() {
    
    ::GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    if (wchar_t* slash = ::wcsrchr(g_dir, L'\\'))
        *slash = L'\0';

    g_prevFilter = ::SetUnhandledExceptionFilter(OnCrash);   // 挂 OnCrash
    std::set_terminate(OnTerminate);                               // 挂 OnTerminate
}

void UninstallCrashHandler() {
    ::SetUnhandledExceptionFilter(g_prevFilter);   // 换回挂之前那个(可能是系统默认)
    g_prevFilter = nullptr;
    std::set_terminate(nullptr);                   // 交回给 CRT 默认行为
}
