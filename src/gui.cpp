// gui.cpp - AutoSlice-download 原生 Win32 图形界面
// 直接复用 downloader.cpp 引擎: 进度回调驱动进度条, cancel_flag 支持取消
// 构建: 见 CMakeLists.txt (downloadergui target), 入口 wWinMain
#include "downloader.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")

// ===== 控件 ID =====
enum {
    IDC_URL_EDIT = 101,
    IDC_PATH_EDIT,
    IDC_THREADS_EDIT,
    IDC_SPEED_EDIT,
    IDC_RETRY_EDIT,
    IDC_RETRY_DELAY_EDIT,
    IDC_START_BTN,
    IDC_CANCEL_BTN,
    IDC_BROWSE_BTN,
    IDC_PROGRESS,
    IDC_STATUS_TEXT,
    IDC_LOG_EDIT,
};

// ===== 自定义消息 =====
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_ERROR    (WM_APP + 2)
#define WM_APP_DONE     (WM_APP + 3)

// ===== 全局状态 (UI 线程与下载线程共享) =====
static HWND g_hwnd = nullptr;
static std::thread g_thread;
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_cancel{false};

// 下载配置 (UI 线程读取输入后填入, 下载线程只读)
static std::vector<std::string> g_urls;
static std::string g_path;
static pcl_dl::DownloadOptions g_opt;

// 进度/错误 (下载线程写入, UI 线程通过消息读取)
static std::atomic<std::int64_t> g_done{0};
static std::atomic<std::int64_t> g_total{0};
static std::atomic<std::int64_t> g_speed{0};
static std::string g_error;
static std::atomic<std::int64_t> g_last_post_ms{0};

// ===== UTF-8 <-> UTF-16 =====
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ===== 控件文本读写 =====
static std::wstring GetEditText(int id) {
    HWND h = GetDlgItem(g_hwnd, id);
    int len = GetWindowTextLengthW(h);
    std::wstring w(len + 1, 0);
    GetWindowTextW(h, &w[0], len + 1);
    w.resize(len);
    return w;
}

static void SetStatus(const std::wstring& text) {
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_STATUS_TEXT), text.c_str());
}

// 日志区追加 (只读多行编辑框)
static void AppendLog(const std::wstring& line) {
    HWND hLog = GetDlgItem(g_hwnd, IDC_LOG_EDIT);
    std::wstring msg = line + L"\r\n";
    SendMessageW(hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)msg.c_str());
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);
}

// ===== 进度回调 (调度线程调用) → 节流后通知 UI =====
static void OnProgress(std::int64_t done, std::int64_t total, std::int64_t speed) {
    g_done = done;
    g_total = total;
    g_speed = speed;
    auto now = GetTickCount64();
    if (now - g_last_post_ms.load() >= 100) {
        g_last_post_ms = now;
        PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, 0);
    }
}

// ===== 下载线程 =====
static void DownloadThreadFunc() {
    g_opt.quiet = true;
    g_opt.cancel_flag = &g_cancel;
    g_opt.on_progress = OnProgress;
    g_opt.on_error = [](const std::string& e) {
        g_error = e;
        PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
    };
    bool ok = pcl_dl::DownloadEngine::Download(g_urls, g_path, g_opt);
    g_busy = false;
    PostMessageW(g_hwnd, WM_APP_DONE, ok ? 1 : 0, 0);
}

// ===== 解析高级选项 =====
static int ParseIntEdit(int id, int fallback) {
    std::wstring w = GetEditText(id);
    if (w.empty()) return fallback;
    return _wtoi(w.c_str());
}

// ===== 点击"开始下载" =====
static void StartDownload(HWND hwnd) {
    if (g_busy.load()) return;

    // 1. 读取 URL (每行一个)
    std::vector<std::string> urls;
    std::wstring urlText = GetEditText(IDC_URL_EDIT);
    size_t pos = 0;
    while (pos <= urlText.size()) {
        size_t eol = urlText.find_first_of(L"\r\n", pos);
        std::wstring line = urlText.substr(pos, (eol == std::wstring::npos ? urlText.size() : eol) - pos);
        if (!line.empty()) urls.push_back(WideToUtf8(line));
        if (eol == std::wstring::npos) break;
        pos = eol + 1;
    }
    // 2. 输出路径
    std::string path = WideToUtf8(GetEditText(IDC_PATH_EDIT));
    // 3. 校验
    if (urls.empty()) {
        MessageBoxW(hwnd, L"请输入至少一个下载地址", L"AutoSlice", MB_OK | MB_ICONWARNING);
        return;
    }
    if (path.empty()) {
        // 默认输出名 = 第一个 URL 的文件名
        std::string u = urls[0];
        auto q = u.find_first_of("?#");
        if (q != std::string::npos) u = u.substr(0, q);
        auto slash = u.find_last_of('/');
        if (slash != std::string::npos) u = u.substr(slash + 1);
        path = u.empty() ? "download.bin" : u;
    }

    // 4. 高级选项
    pcl_dl::DownloadOptions opt;
    opt.max_threads = ParseIntEdit(IDC_THREADS_EDIT, 0);
    opt.speed_cap = (std::int64_t)ParseIntEdit(IDC_SPEED_EDIT, -1) * 1024;
    if (opt.speed_cap < 0) opt.speed_cap = -1;
    opt.max_retries = std::max(1, ParseIntEdit(IDC_RETRY_EDIT, 3));
    opt.retry_delay_ms = std::max(0, ParseIntEdit(IDC_RETRY_DELAY_EDIT, 1000));

    // 5. 启动
    g_urls = std::move(urls);
    g_path = path;
    g_opt = opt;
    g_cancel = false;
    g_error.clear();
    g_done = 0; g_total = 0; g_speed = 0;

    SetStatus(L"探测文件大小...");
    HWND hLog = GetDlgItem(hwnd, IDC_LOG_EDIT);
    SetWindowTextW(hLog, L"");
    AppendLog(L"开始下载: " + Utf8ToWide(g_path));
    for (auto& u : g_urls) AppendLog(L"  源: " + Utf8ToWide(u));

    // 清进度条
    SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS), PBM_SETPOS, 0, 0);

    g_busy = true;
    g_thread = std::thread(DownloadThreadFunc);
}

// ===== 点击"取消" =====
static void CancelDownload(HWND hwnd) {
    if (!g_busy.load()) return;
    g_cancel = true;
    SetStatus(L"正在取消, 清理分片...");
    EnableWindow(GetDlgItem(hwnd, IDC_CANCEL_BTN), FALSE);
}

// ===== 窗口过程 =====
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            // 创建字体
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            auto NewEdit = [&](DWORD style, int id, int x, int y, int w, int hgt) {
                HWND hed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style, x, y, w, hgt,
                                           hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(hed, WM_SETFONT, (WPARAM)hFont, TRUE);
                return hed;
            };
            auto NewLabel = [&](const wchar_t* text, int x, int y, int w, int hgt) {
                HWND hx = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, hgt,
                                          hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(hx, WM_SETFONT, (WPARAM)hFont, TRUE);
                return hx;
            };
            auto NewButton = [&](const wchar_t* text, int id, int x, int y, int w, int hgt) {
                HWND hx = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          x, y, w, hgt, hwnd, (HMENU)(INT_PTR)id,
                                          GetModuleHandleW(nullptr), nullptr);
                SendMessageW(hx, WM_SETFONT, (WPARAM)hFont, TRUE);
                return hx;
            };

            NewLabel(L"下载地址 (每行一个, 支持多源):", 12, 8, 300, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                    IDC_URL_EDIT, 12, 28, 496, 62);
            NewLabel(L"输出路径 (留空 = 自动取 URL 文件名):", 12, 98, 300, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    IDC_PATH_EDIT, 12, 118, 440, 22);
            NewButton(L"浏览...", IDC_BROWSE_BTN, 458, 117, 50, 24);

            // 高级选项行
            NewLabel(L"线程上限", 12, 148, 56, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_THREADS_EDIT, 68, 148, 44, 22);
            NewLabel(L"0=自动", 116, 148, 44, 18);
            NewLabel(L"限速KB/s", 164, 148, 56, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_SPEED_EDIT, 220, 148, 50, 22);
            NewLabel(L"-1不限", 274, 148, 44, 18);
            NewLabel(L"重试", 322, 148, 34, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_RETRY_EDIT, 356, 148, 40, 22);
            NewLabel(L"间隔ms", 400, 148, 44, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_RETRY_DELAY_EDIT, 448, 148, 60, 22);

            NewButton(L"开始下载", IDC_START_BTN, 12, 180, 100, 30);
            NewButton(L"取消", IDC_CANCEL_BTN, 120, 180, 80, 30);

            // 进度条
            HWND hBar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                        12, 220, 496, 22, hwnd, (HMENU)(INT_PTR)IDC_PROGRESS,
                                        GetModuleHandleW(nullptr), nullptr);
            SendMessageW(hBar, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hBar, PBM_SETRANGE32, 0, 1000);

            NewLabel(L"状态:", 12, 250, 40, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | ES_READONLY, IDC_STATUS_TEXT, 56, 250, 452, 20);

            NewLabel(L"日志:", 12, 278, 60, 18);
            NewEdit(WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | WS_VSCROLL, IDC_LOG_EDIT, 12, 298, 496, 130);

            // 默认值
            SetWindowTextW(GetDlgItem(hwnd, IDC_THREADS_EDIT), L"0");
            SetWindowTextW(GetDlgItem(hwnd, IDC_SPEED_EDIT), L"-1");
            SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY_EDIT), L"3");
            SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY_DELAY_EDIT), L"1000");
            SetStatus(L"就绪");
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_START_BTN) {
                StartDownload(hwnd);
            } else if (id == IDC_CANCEL_BTN) {
                CancelDownload(hwnd);
            } else if (id == IDC_BROWSE_BTN) {
                // 保存对话框选择输出路径; 默认文件名与下载文件一致
                wchar_t path[MAX_PATH] = {};
                std::wstring curPath = GetEditText(IDC_PATH_EDIT);
                if (!curPath.empty()) {
                    // 路径框已有内容 → 直接用作默认 (保留用户已填的目录/文件名)
                    wcsncpy(path, curPath.c_str(), MAX_PATH - 1);
                } else {
                    // 从第一个 URL 提取文件名 (去掉 query/fragment)
                    std::wstring urlText = GetEditText(IDC_URL_EDIT);
                    std::wstring url = urlText.substr(0, urlText.find_first_of(L"\r\n"));
                    auto q = url.find_first_of(L"?#");
                    if (q != std::wstring::npos) url = url.substr(0, q);
                    auto slash = url.find_last_of(L'/');
                    if (slash != std::wstring::npos) url = url.substr(slash + 1);
                    if (url.empty()) url = L"download.bin";
                    wcsncpy(path, url.c_str(), MAX_PATH - 1);
                }
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0\0";
                ofn.lpstrDefExt = L"bin";
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                if (GetSaveFileNameW(&ofn)) {
                    SetWindowTextW(GetDlgItem(hwnd, IDC_PATH_EDIT), path);
                }
            }
            return 0;
        }

        case WM_APP_PROGRESS: {
            std::int64_t done = g_done.load(), total = g_total.load(), speed = g_speed.load();
            wchar_t buf[256];
            HWND hBar = GetDlgItem(hwnd, IDC_PROGRESS);
            if (total > 0) {
                double pct = done * 100.0 / total;
                SendMessageW(hBar, PBM_SETPOS, (WPARAM)(pct * 10), 0);
                swprintf(buf, 256, L"%.2f%%  %.2f MB / %.2f MB | %.1f KB/s",
                         pct, done / 1048576.0, total / 1048576.0, speed / 1024.0);
            } else {
                // 未知大小 (chunked): 进度条按已下载量增长 (每 100MB 满)
                SendMessageW(hBar, PBM_SETPOS, (WPARAM)std::min<std::int64_t>(1000, done / 102400), 0);
                swprintf(buf, 256, L"%.2f MB | %.1f KB/s (未知大小)",
                         done / 1048576.0, speed / 1024.0);
            }
            SetStatus(buf);
            return 0;
        }

        case WM_APP_ERROR: {
            SetStatus(L"错误: " + Utf8ToWide(g_error));
            AppendLog(L"错误: " + Utf8ToWide(g_error));
            return 0;
        }

        case WM_APP_DONE: {
            bool ok = (wp == 1);
            EnableWindow(GetDlgItem(hwnd, IDC_CANCEL_BTN), TRUE);
            if (ok) {
                SetStatus(L"完成");
                AppendLog(L"完成: " + Utf8ToWide(g_path));
            } else {
                if (g_cancel.load())
                    SetStatus(L"已取消 (分片已清理)");
                AppendLog(L"失败");
            }
            return 0;
        }

        case WM_CLOSE: {
            if (g_busy.load()) {
                if (MessageBoxW(hwnd, L"下载正在进行, 确定退出? (将取消下载并清理分片)",
                                L"AutoSlice", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                g_cancel = true;
                if (g_thread.joinable()) g_thread.join();
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"AutoSliceWindow";
    // 背景刷: 未设置时默认黑色 (Windows 经典坑), 显式用系统窗口色
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"AutoSliceWindow", L"AutoSlice-download",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 536, 470,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
