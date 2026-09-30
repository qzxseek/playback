/* @Created On : 2026/9/30
   @Author : 孟源
   @note : 日志实现 —— 平台无关(除了取 exe 目录那一段)。
*/
#include "audio_sdk/logging.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  include <windows.h>      // GetModuleFileNameW
#endif

namespace {

std::mutex    g_mutex;
std::ofstream g_file;

/** @brief 获取日志目录
 *  @return 日志所在目录
 *  @note Windows: 调用方 exe 所在目录, 和 crash_*.dmp 落同一处, 排查时一起拿。
 *                  (GetModuleFileNameW(nullptr,...) 给的是【exe】路径, 不是本 DLL 的)
 *        Android: 没有"exe 目录"这回事 —— cwd 是 / , 不可写。改从 /proc/self/cmdline
 *                 取包名, 落到 /data/data/<包名>/files: 系统安装时就建好的 App 私有目录,
 *                 自身可读写、不需要任何权限。
 */
std::string LogDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0)
        return std::filesystem::path(buf).parent_path().u8string();
#elif defined(__ANDROID__)
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string pkg;
    std::getline(f, pkg, '\0');                  // cmdline 是 NUL 分隔的, 第一段就是进程名
    const size_t colon = pkg.find(':');          // 非主进程叫 "com.foo:remote", 砍掉后缀
    if (colon != std::string::npos) pkg.resize(colon);
    if (!pkg.empty()) return "/data/data/" + pkg + "/files";
#endif
    return ".";   // 兜底: 打不开就静默不写(见 FileLocked)
}

// 定宽 5 字符, 日志里的级别列才对得齐
const char* LevelName(AudioSdk::LogLevel lv) {
    switch (lv) {
        case AudioSdk::LogLevel::Debug: return "DEBUG";
        case AudioSdk::LogLevel::Info:  return "INFO ";
        case AudioSdk::LogLevel::Warn:  return "WARN ";
        case AudioSdk::LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void FillLocalTime(std::time_t t, std::tm& tm) {
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
}

/** @brief 获取当天的文件流
 *  @return 当天的文件流
 */
std::ofstream& FileLocked() {
    if (g_file.is_open()) return g_file;

    std::tm tm = {};
    FillLocalTime(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()), tm);

    char name[64] = {};
    std::strftime(name, sizeof(name), "audio_sdk_%Y%m%d.txt", &tm);   // 一天一个文件

    const std::string dir = LogDir();
    // u8path: dir 是 UTF-8(路径可能含中文), 直接给 path 才不会乱码
    const std::filesystem::path path = std::filesystem::u8path(dir) / name;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);      // 目录不在就建

    g_file.open(path, std::ios::out | std::ios::app);
    return g_file;
}

}   // namespace

namespace AudioSdk {

void LogWrite(LogLevel level, const char* tag, const char* fmt, ...) {
    if (!fmt) return;

    char msg[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // 时间戳
    const auto now = std::chrono::system_clock::now();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch()) % 1000;
    std::tm tm = {};
    FillLocalTime(std::chrono::system_clock::to_time_t(now), tm);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::ofstream& f = FileLocked();
    if (!f.is_open()) return;                      

    f << stamp << '.' << std::setfill('0') << std::setw(3) << ms.count()
      << " [" << LevelName(level) << "] "
      << (tag ? tag : "-") << "  " << msg << '\n';
    f.flush();                                     
}

}   // namespace AudioSdk
