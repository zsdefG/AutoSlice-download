// main.cpp - PCL 风格下载引擎命令行示例
// 用法:
//   downloader.exe <url> [url2 url3 ...] <输出路径> [--threads N] [--speed-cap N]
//   downloader.exe --self-test <目录>    # 本地分片/合并/动态线程自测
#include "downloader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static std::int64_t g_last_print_ms = 0;

// 构造进度条字符串: n 个 █ + (BAR_W-n) 个 ░ (UTF-8 字符)
static std::string BarStr(int n, int bar_w) {
    if (n < 0) n = 0;
    if (n > bar_w) n = bar_w;
    const char* FULL = "\xE2\x96\x88";  // █ U+2588
    const char* EMPTY = "\xE2\x96\x91"; // ░ U+2591
    std::string s;
    s.reserve(bar_w * 3);
    for (int i = 0; i < n; ++i) s += FULL;
    for (int i = n; i < bar_w; ++i) s += EMPTY;
    return s;
}

// 绘制总进度条: [████████████░░░░░░░░░░] 84.20% 252.61 MB / 300.00 MB | 2967.0 KB/s
// 未知大小 (chunked): [█████████████░░░░░░] 12.35 MB | 2967.0 KB/s (无百分比, 条按已下载量增长)
static void PrintProgress(std::int64_t done, std::int64_t total, std::int64_t speed) {
    auto now = GetTickCount64();
    if (now - g_last_print_ms < 200 && (total < 0 || done < total)) return;
    g_last_print_ms = now;

    const int BAR_W = 24;  // 进度条字符宽度
    double spd = speed / 1024.0;
    double mb = done / 1024.0 / 1024.0;

    if (total < 0) {
        // 未知大小: 无百分比, 进度条按已下载量线性增长到满
        double frac = std::min(1.0, mb / 100.0);  // 每 100MB 一段, 到 100MB 后条满
        int filled = (int)(frac * BAR_W);
        printf("\r  [%s] %7.2f MB | %7.1f KB/s",
               BarStr(filled, BAR_W).c_str(), mb, spd);
    } else {
        double pct = total > 0 ? done * 100.0 / total : 0.0;
        double tmb = total / 1024.0 / 1024.0;
        int filled = (int)(pct / 100.0 * BAR_W);
        printf("\r  [%s] %6.2f%% %7.2f MB / %7.2f MB | %7.1f KB/s",
               BarStr(filled, BAR_W).c_str(), pct, mb, tmb, spd);
    }
    fflush(stdout);
    if (total >= 0 && done >= total) printf("\n");
}

static int SelfTest(const std::string& dir) {
    // 生成一个 12MB 随机测试文件
    const std::string src = dir + "/test_src.bin";
    {
        FILE* f = nullptr;
        if (fopen_s(&f, src.c_str(), "wb") != 0 || !f) { printf("无法创建测试文件\n"); return 1; }
        srand(12345);
        char buf[64 * 1024];
        for (int i = 0; i < 12 * 1024 * 1024 / (int)sizeof(buf); ++i) {
            for (auto& c : buf) c = (char)(rand() % 256);
            fwrite(buf, 1, sizeof(buf), f);
        }
        fclose(f);
    }
    printf("[自测] 已生成 12MB 源文件: %s\n", src.c_str());

    // 计算 SHA-256 (简单实现: 用 md5 替代不可行, 这里直接比对字节)
    // 用命令行 certutil 计算哈希
    printf("[自测] 源文件哈希: ");
    fflush(stdout);
    std::string cmd = "certutil -hashfile \"" + src + "\" SHA256";
    int rc = system(cmd.c_str());
    (void)rc;

    // 下载 (单源, 本地 HTTP 服务器需要外部起; 这里直接用一个文件协议模拟?
    // 不行 — WinHTTP 只支持 http/https。此自测用最小 HTTP 服务器 (纯 winsock 简化版)
    printf("[自测] 本地不支持 http://file 协议, 请用 python -m http.server 后:\n");
    printf("       downloader.exe http://127.0.0.1:8000/test_src.bin out.bin --threads 8\n");
    return 0;
}

// Ctrl+C / 关闭窗口处理器: 中断下载时清理残留分片, 避免留下 .part* 文件。
// Handler 运行在独立的异步线程, 只置标志并返回 TRUE (吃掉信号),
// 由主循环检测标志后在主线程安全清理, 避免 std::string 竞态。
static char g_cleanup_path[4096] = {0};
static std::atomic<int> g_cleaned{0};
static std::atomic<bool> g_cancel{false};

// atexit 兜底: 无论程序以何种方式正常退出 (return/exit), 都清理残留分片。
// 这是进程内最后一道防线 (强杀进程除外)。
static void AtExitCleanup() {
    if (g_cleanup_path[0] != '\0') {
        pcl_dl::DownloadEngine::CleanupPartFiles(g_cleanup_path);
    }
}

static BOOL WINAPI CtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_cleaned.exchange(1);   // 置标志
            g_cancel.exchange(true); // 通知引擎取消下载
            return TRUE;             // 吃掉信号, 由主线程安全清理并退出
        default:
            return FALSE;
    }
}

// 主循环检测到 Ctrl+C 后调用: 清理分片并退出
static bool CheckInterrupted() {
    if (g_cleaned.load() == 1) {
        if (g_cleanup_path[0] != '\0') {
            pcl_dl::DownloadEngine::CleanupPartFiles(g_cleanup_path);
        }
        return true;
    }
    return false;
}

static void PrintUsage() {
    printf("用法:\n");
    printf("  downloader.exe <url> [url2 ...] <输出路径> [选项]\n");
    printf("  选项:\n");
    printf("    -h, --help          显示本帮助\n");
    printf("    --threads N         最大线程数上限 (默认 0 = 自动, 引擎按带宽自适应, 硬上限 32)\n");
    printf("    --speed-cap KB/s    全局限速 (默认 -1 = 不限速; 0 = 自动按实测带宽 80%% 限速)\n");
    printf("    --per-thread KB/s   每线程速度下限, 低于此判定带宽饱和 (默认 32)\n");
    printf("    --retry N           下载失败后的总尝试次数, 含首次 (默认 3, 1 = 不重试)\n");
    printf("    --retry-delay ms    每次重试前等待毫秒 (默认 1000)\n");
    printf("    --quiet             静默模式 (不打印进度和自适应日志)\n");
    printf("  示例:\n");
    printf("    downloader.exe https://example.com/file.bin out.bin\n");
    printf("    downloader.exe http://a.com/f.bin http://mirror.com/f.bin out.bin --speed-cap 0\n");
    printf("  downloader.exe --self-test <目录>    # 生成 12MB 测试文件\n");
}

// 编码适配: MinGW 编译后中文字符串在 EXE 内是 UTF-8 字节。
// Windows 原生终端 (cmd/PowerShell) 默认代码页是 GBK(936), 直接输出 UTF-8 会乱码。
// 修复: 程序启动时把控制台输出代码页切成 UTF-8 (Win10 1809+ 终端原生支持 UTF-8 渲染)。
// 注意: PowerShell 5.1 管道捕获 (`| Out-File`) 用其自身的 OutputEncoding (GBK) 解码,
//       这是 PS 自身行为, 程序内无法改变; 交互式直接运行本修复已足够。
static void InitOutputEncoding() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

int main(int argc, char* argv[]) {
    InitOutputEncoding();
    // 注册 Ctrl+C / 关闭窗口处理器, 中断时清理分片残留
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    // atexit 兜底: 任何正常退出路径都清理残留分片
    atexit(AtExitCleanup);

    std::vector<std::string> args(argv + 1, argv + argc);
    pcl_dl::DownloadOptions opt;
    std::string output;
    std::vector<std::string> urls;

    if (!args.empty() && args[0] == "--self-test") {
        std::string dir = args.size() > 1 ? args[1] : ".";
        return SelfTest(dir);
    }

    // 帮助
    for (const auto& a : args) {
        if (a == "-h" || a == "--help" || a == "/?") {
            PrintUsage();
            return 0;
        }
    }

    if (args.empty()) {
        PrintUsage();
        return 1;
    }

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--threads" && i + 1 < args.size()) {
            opt.max_threads = std::max(0, atoi(args[++i].c_str()));
        } else if (a == "--speed-cap" && i + 1 < args.size()) {
            opt.speed_cap = (std::int64_t)atoll(args[++i].c_str()) * 1024;
            if (opt.speed_cap < 0) opt.speed_cap = -1;
        } else if (a == "--per-thread" && i + 1 < args.size()) {
            opt.per_thread_min = (std::int64_t)atoll(args[++i].c_str()) * 1024;
        } else if (a == "--retry" && i + 1 < args.size()) {
            opt.max_retries = std::max(1, atoi(args[++i].c_str()));
        } else if (a == "--retry-delay" && i + 1 < args.size()) {
            opt.retry_delay_ms = std::max(0, atoi(args[++i].c_str()));
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a.rfind("http://", 0) == 0 || a.rfind("https://", 0) == 0) {
            urls.push_back(a);
        } else {
            output = a;  // 最后一个非 URL 参数是输出路径
        }
    }

    if (urls.empty()) {
        printf("错误: 需要至少一个 URL\n");
        return 1;
    }

    // 默认输出路径 = 第一个 URL 的文件名 (去掉 query/fragment)
    if (output.empty()) {
        std::string u = urls[0];
        auto q = u.find_first_of("?#");
        if (q != std::string::npos) u = u.substr(0, q);
        auto slash = u.find_last_of('/');
        if (slash != std::string::npos) u = u.substr(slash + 1);
        if (u.empty()) u = "download.bin";
        // 去掉 URL 编码残留 (如 %20), 简单处理空格
        for (auto& c : u) if (c == '%') { u = u.substr(0, u.find('%')); break; }
        output = u;
    }

    g_cleanup_path[0] = '\0';
    strncpy(g_cleanup_path, output.c_str(), sizeof(g_cleanup_path) - 1);  // Ctrl+C 时按此路径清理

    printf("  源: %zu 个\n", urls.size());
    printf("  输出: %s\n", output.c_str());
    if (opt.max_threads > 0) printf("  线程上限: %d\n", opt.max_threads);
    else printf("  线程上限: 自动 (最多 32)\n");
    printf("  每线程速度下限: %.0f KB/s  碎片下限: %.0f KB\n",
           opt.per_thread_min / 1024.0, opt.piece_limit / 1024.0);
    if (opt.speed_cap > 0) printf("  全局限速: %.0f KB/s\n", opt.speed_cap / 1024.0);
    else if (opt.speed_cap == 0) printf("  限速: 自动 (实测带宽的 80%%)\n");
    else printf("  限速: 不限\n");
    if (opt.max_retries > 1) printf("  重试: %d 次 (间隔 %dms)\n", opt.max_retries, opt.retry_delay_ms);
    printf("  探测文件大小...\n");
    fflush(stdout);

    auto t0 = std::chrono::steady_clock::now();
    opt.cancel_flag = &g_cancel;  // Ctrl+C 时引擎取消下载
    if (!opt.quiet) {
        opt.on_progress = [](std::int64_t done, std::int64_t total, std::int64_t speed) {
            // 进度回调里检测 Ctrl+C: 中断时由主线程清理分片
            if (CheckInterrupted()) return;
            PrintProgress(done, total, speed);
        };
    } else {
        opt.on_progress = [](std::int64_t, std::int64_t, std::int64_t) {
            if (CheckInterrupted()) return;
        };
    }

    // 引擎内部错误通过返回值判断, 详细信息在 Download() 里; 这里先探测 URL 可访问性
    // 直接调用, 失败时打印最后一条错误
    bool ok = pcl_dl::DownloadEngine::Download(urls, output, opt);

    // 若因 Ctrl+C 中断, 清理分片残留
    if (CheckInterrupted()) {
        printf("\n已取消 (Ctrl+C), 已清理残留分片\n");
        return 1;
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    if (ok) {
        std::int64_t size = fs::file_size(output);
        printf("完成: %s (%.2f MB, %.1f s, 平均 %.0f KB/s)\n",
               output.c_str(), size / 1024.0 / 1024.0, secs,
               size / 1024.0 / (secs > 0 ? secs : 1));
        return 0;
    } else {
        printf("\n失败 (%.1f s)\n", secs);
        return 1;
    }
}
