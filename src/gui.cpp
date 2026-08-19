// gui.cpp - AutoSlice-download 原生 Win32 GUI (v2: 新设计落地)
// 卡片式分区 + LED 状态灯 + 指标区 + 可调窗口 (响应式布局)
// 直接复用 downloader.cpp 引擎: 进度回调驱动 UI, cancel_flag 支持取消
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
    IDC_BROWSE_BTN,
    IDC_THREADS_EDIT,
    IDC_SPEED_EDIT,
    IDC_RETRY_EDIT,
    IDC_RETRY_DELAY_EDIT,
    IDC_START_BTN,
    IDC_CANCEL_BTN,
    IDC_LED,
    IDC_STATUS_TEXT,
    IDC_PROGRESS,
    IDC_M_DONE,
    IDC_M_SPEED,
    IDC_M_ETA,
    IDC_M_THREADS,
    IDC_LOG_EDIT,
    // 标签
    IDC_L_URL = 1901,
    IDC_L_SAVE,
    IDC_L_THREADS,
    IDC_L_SPEED,
    IDC_L_RETRY,
    IDC_L_DELAY,
    IDC_L_DONE,
    IDC_L_SPEED2,
    IDC_L_ETA,
    IDC_L_THREADS2,
    IDC_L_LOG,
};

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_ERROR    (WM_APP + 2)
#define WM_APP_DONE     (WM_APP + 3)

// ===== 全局状态 =====
static HWND g_hwnd = nullptr;
static std::thread g_thread;
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_cancel{false};
static std::vector<std::string> g_urls;
static std::string g_path;
static pcl_dl::DownloadOptions g_opt;
static std::string g_error;
static std::atomic<std::int64_t> g_done{0}, g_total{0}, g_speed{0}, g_last_post_ms{0};

// LED 状态色 (空闲灰 / 下载中蓝 / 完成绿 / 失败红 / 取消琥珀)
static COLORREF g_ledColor = RGB(140, 144, 150);
static std::wstring g_ledText = L"就绪";

// ===== 工具 =====
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
static void SetText(int id, const std::wstring& s) {
    SetWindowTextW(GetDlgItem(g_hwnd, id), s.c_str());
}
static std::wstring GetText(int id) {
    HWND h = GetDlgItem(g_hwnd, id);
    int len = GetWindowTextLengthW(h);
    std::wstring w(len + 1, 0);
    GetWindowTextW(h, &w[0], len + 1);
    w.resize(len);
    return w;
}
static std::wstring FmtBytes(double b) {
    wchar_t buf[64];
    if (b >= 1024.0 * 1024 * 1024) swprintf(buf, 64, L"%.2f GB", b / 1073741824.0);
    else swprintf(buf, 64, L"%.2f MB", b / 1048576.0);
    return buf;
}
static void AppendLog(const std::wstring& line) {
    HWND hLog = GetDlgItem(g_hwnd, IDC_LOG_EDIT);
    std::wstring msg = line + L"\r\n";
    SendMessageW(hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)msg.c_str());
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);
}
static void SetLed(COLORREF color, const wchar_t* text) {
    g_ledColor = color;
    g_ledText = text;
    InvalidateRect(GetDlgItem(g_hwnd, IDC_LED), nullptr, TRUE);  // 重绘 LED
    SetText(IDC_STATUS_TEXT, text);
}
static void SetBusy(bool busy) {
    g_busy = busy;
    EnableWindow(GetDlgItem(g_hwnd, IDC_START_BTN), !busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_CANCEL_BTN), busy);
}

// ===== 进度回调 (调度线程) =====
static void OnProgress(std::int64_t done, std::int64_t total, std::int64_t speed) {
    g_done = done; g_total = total; g_speed = speed;
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

// ===== 布局 (响应式: WM_SIZE 时重排) =====
static void Layout(HWND hwnd, int W, int H) {
    const int M = 14;                    // 边距
    int y = 12;
    int cw = W - M * 2;                  // 内容宽度
    auto MV = [&](int id, int x, int yy, int w, int h) {
        MoveWindow(GetDlgItem(hwnd, id), x, yy, w, h, TRUE);
    };

    // 下载源
    MV(IDC_L_URL, M, y - 4, 300, 16);
    MV(IDC_URL_EDIT, M, y + 14, cw, 54);
    y += 76;

    // 保存位置
    MV(IDC_L_SAVE, M, y - 4, 200, 16);
    MV(IDC_PATH_EDIT, M, y + 14, cw - 70, 24);
    MV(IDC_BROWSE_BTN, M + cw - 62, y + 13, 62, 26);
    y += 44;

    // 高级选项 (窄窗口折行 2x2)
    bool narrow = cw < 560;
    if (narrow) {
        int gw = (cw - 10) / 2;
        int lh = 44;
        MV(IDC_L_THREADS, M, y - 2, gw, 14);
        MV(IDC_THREADS_EDIT, M, y + 14, gw, 22);
        MV(IDC_L_SPEED, M + gw + 10, y - 2, gw, 14);
        MV(IDC_SPEED_EDIT, M + gw + 10, y + 14, gw, 22);
        MV(IDC_L_RETRY, M, y + 14 + lh - 2, gw, 14);
        MV(IDC_RETRY_EDIT, M, y + 14 + lh + 12, gw, 22);
        MV(IDC_L_DELAY, M + gw + 10, y + 14 + lh - 2, gw, 14);
        MV(IDC_RETRY_DELAY_EDIT, M + gw + 10, y + 14 + lh + 12, gw, 22);
        y += 14 + lh + 34 + 12;
    } else {
        int gw = (cw - 30) / 4;
        for (int i = 0; i < 4; ++i) {
            int x = M + (gw + 10) * i;
            int lid = (i == 0) ? IDC_L_THREADS : (i == 1) ? IDC_L_SPEED : (i == 2) ? IDC_L_RETRY : IDC_L_DELAY;
            int eid = (i == 0) ? IDC_THREADS_EDIT : (i == 1) ? IDC_SPEED_EDIT : (i == 2) ? IDC_RETRY_EDIT : IDC_RETRY_DELAY_EDIT;
            MV(lid, x, y - 2, gw, 14);
            MV(eid, x, y + 14, gw, 22);
        }
        y += 42;
    }

    // 按钮
    MV(IDC_START_BTN, M, y, 120, 32);
    MV(IDC_CANCEL_BTN, M + 128, y, 88, 32);
    y += 42;

    // 进度条
    MV(IDC_PROGRESS, M, y, cw, 12);
    y += 22;

    // 指标区 (4 列: 值 + 标签)
    int mw = (cw - 30) / 4;
    int mid[4][2] = {
        {IDC_M_DONE, IDC_L_DONE}, {IDC_M_SPEED, IDC_L_SPEED2},
        {IDC_M_ETA, IDC_L_ETA}, {IDC_M_THREADS, IDC_L_THREADS2},
    };
    for (int i = 0; i < 4; ++i) {
        int x = M + (mw + 10) * i;
        MV(mid[i][0], x, y, mw, 18);
        MV(mid[i][1], x, y + 18, mw, 14);
    }
    y += 38;

    // 状态行 (LED + 文本)
    MV(IDC_LED, M, y + 1, 16, 16);
    MV(IDC_STATUS_TEXT, M + 24, y, cw - 24, 20);
    y += 28;

    // 日志区 (弹性占满剩余)
    MV(IDC_L_LOG, M, y - 2, 100, 14);
    int logTop = y + 12;
    int logH = H - logTop - M;
    if (logH < 40) logH = 40;
    MV(IDC_LOG_EDIT, M, logTop, cw, logH);
}

// ===== LED 自绘 (状态灯) =====
static void DrawLed(HWND hwnd, HDC hdc, RECT* rc) {
    HBRUSH br = CreateSolidBrush(g_ledColor);
    HBRUSH old = (HBRUSH)SelectObject(hdc, br);
    Ellipse(hdc, rc->left, rc->top, rc->right, rc->bottom);
    SelectObject(hdc, old);
    DeleteObject(br);
}

// ===== 窗口过程 =====
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            auto New = [&](const wchar_t* cls, DWORD style, int id, DWORD ex = 0) {
                HWND h = CreateWindowExW(ex, cls, L"", style, 0, 0, 0, 0, hwnd,
                                         (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
                SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
                return h;
            };
            auto Label = [&](const wchar_t* text, int id) {
                HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                         0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id,
                                         GetModuleHandleW(nullptr), nullptr);
                SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
                return h;
            };
            // 标签行 (位置由 Layout 控制)
            Label(L"下载地址（每行一个，多源自动容错）", IDC_L_URL);
            Label(L"保存位置", IDC_L_SAVE);
            Label(L"线程上限", IDC_L_THREADS);
            Label(L"限速 KB/s", IDC_L_SPEED);
            Label(L"重试", IDC_L_RETRY);
            Label(L"间隔 ms", IDC_L_DELAY);
            Label(L"已下载", IDC_L_DONE);
            Label(L"速度", IDC_L_SPEED2);
            Label(L"剩余时间", IDC_L_ETA);
            Label(L"线程数", IDC_L_THREADS2);
            Label(L"日志", IDC_L_LOG);

            New(L"EDIT", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                IDC_URL_EDIT, WS_EX_CLIENTEDGE);
            New(L"EDIT", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, IDC_PATH_EDIT, WS_EX_CLIENTEDGE);
            New(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, IDC_BROWSE_BTN);
            New(L"EDIT", WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_THREADS_EDIT, WS_EX_CLIENTEDGE);
            New(L"EDIT", WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_SPEED_EDIT, WS_EX_CLIENTEDGE);
            New(L"EDIT", WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_RETRY_EDIT, WS_EX_CLIENTEDGE);
            New(L"EDIT", WS_CHILD | WS_VISIBLE | WS_TABSTOP, IDC_RETRY_DELAY_EDIT, WS_EX_CLIENTEDGE);
            New(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, IDC_START_BTN);
            New(L"BUTTON", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, IDC_CANCEL_BTN);
            New(L"STATIC", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, IDC_LED);
            New(L"STATIC", WS_CHILD | WS_VISIBLE | SS_LEFT, IDC_STATUS_TEXT);
            New(L"EDIT", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                IDC_LOG_EDIT, WS_EX_CLIENTEDGE);
            New(L"STATIC", WS_CHILD | WS_VISIBLE | SS_CENTER, IDC_M_DONE);
            New(L"STATIC", WS_CHILD | WS_VISIBLE | SS_CENTER, IDC_M_SPEED);
            New(L"STATIC", WS_CHILD | WS_VISIBLE | SS_CENTER, IDC_M_ETA);
            New(L"STATIC", WS_CHILD | WS_VISIBLE | SS_CENTER, IDC_M_THREADS);
            // 进度条
            HWND hBar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                        hwnd, (HMENU)(INT_PTR)IDC_PROGRESS, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(hBar, PBM_SETRANGE32, 0, 1000);

            // 默认值
            SetText(IDC_THREADS_EDIT, L"0");
            SetText(IDC_SPEED_EDIT, L"-1");
            SetText(IDC_RETRY_EDIT, L"3");
            SetText(IDC_RETRY_DELAY_EDIT, L"1000");
            SetText(IDC_M_DONE, L"0 MB");
            SetText(IDC_M_SPEED, L"—");
            SetText(IDC_M_ETA, L"—");
            SetText(IDC_M_THREADS, L"自动");
            SetLed(RGB(140, 144, 150), L"就绪");

            // 按钮文本
            SetWindowTextW(GetDlgItem(hwnd, IDC_START_BTN), L"开始下载");
            SetWindowTextW(GetDlgItem(hwnd, IDC_CANCEL_BTN), L"取消");
            SetWindowTextW(GetDlgItem(hwnd, IDC_BROWSE_BTN), L"浏览...");
            // 输出路径默认留空: 引擎自动取 URL 文件名 (不硬编码本机目录)

            // 标签位置 (固定行, 由 Layout 之后的 MoveWindow 控制)
            // 简化为在 Layout 里同步移动标签
            return 0;
        }

        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            Layout(hwnd, rc.right, rc.bottom);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = 480;
            mmi->ptMinTrackSize.y = 520;
            return 0;
        }

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wp;
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
            return 1;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
            if (di->CtlID == IDC_LED) {
                RECT rc = di->rcItem;
                // 居中圆
                int d = (rc.bottom - rc.top < rc.right - rc.left ? rc.bottom - rc.top : rc.right - rc.left) - 4;
                if (d < 6) d = 6;
                int x = rc.left + (rc.right - rc.left - d) / 2;
                int y = rc.top + (rc.bottom - rc.top - d) / 2;
                HBRUSH br = CreateSolidBrush(g_ledColor);
                HBRUSH old = (HBRUSH)SelectObject(di->hDC, br);
                Ellipse(di->hDC, x, y, x + d, y + d);
                SelectObject(di->hDC, old);
                DeleteObject(br);
                return TRUE;
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_START_BTN) {
                if (g_busy.load()) break;
                std::wstring urlText = GetText(IDC_URL_EDIT);
                g_urls.clear();
                size_t pos = 0;
                while (pos <= urlText.size()) {
                    size_t eol = urlText.find_first_of(L"\r\n", pos);
                    std::wstring line = urlText.substr(pos, (eol == std::wstring::npos ? urlText.size() : eol) - pos);
                    if (!line.empty()) g_urls.push_back(WideToUtf8(line));
                    if (eol == std::wstring::npos) break;
                    pos = eol + 1;
                }
                g_path = WideToUtf8(GetText(IDC_PATH_EDIT));
                if (g_path.empty() && !g_urls.empty()) {
                    std::string u = g_urls[0];
                    auto q = u.find_first_of("?#");
                    if (q != std::string::npos) u = u.substr(0, q);
                    auto slash = u.find_last_of('/');
                    if (slash != std::string::npos) u = u.substr(slash + 1);
                    g_path = u.empty() ? "download.bin" : u;
                }
                if (g_urls.empty()) {
                    MessageBoxW(hwnd, L"请输入至少一个下载地址", L"AutoSlice", MB_OK | MB_ICONWARNING);
                    break;
                }
                pcl_dl::DownloadOptions opt;
                opt.max_threads = _wtoi(GetText(IDC_THREADS_EDIT).c_str());
                opt.speed_cap = (std::int64_t)_wtoi(GetText(IDC_SPEED_EDIT).c_str()) * 1024;
                if (opt.speed_cap < 0) opt.speed_cap = -1;
                opt.max_retries = std::max(1, _wtoi(GetText(IDC_RETRY_EDIT).c_str()));
                opt.retry_delay_ms = std::max(0, _wtoi(GetText(IDC_RETRY_DELAY_EDIT).c_str()));
                g_opt = opt;
                g_cancel = false;
                g_error.clear();
                g_done = g_total = g_speed = 0;
                SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS), PBM_SETPOS, 0, 0);
                SetText(IDC_M_DONE, L"0 MB");
                SetText(IDC_M_SPEED, L"—");
                SetText(IDC_M_ETA, L"—");
                SetWindowTextW(GetDlgItem(hwnd, IDC_LOG_EDIT), L"");
                SetLed(RGB(25, 95, 168), L"下载中");
                AppendLog(L"开始下载: " + Utf8ToWide(g_path));
                for (auto& u : g_urls) AppendLog(L"  源: " + Utf8ToWide(u));
                AppendLog(L"探测文件大小...");
                SetBusy(true);
                g_thread = std::thread(DownloadThreadFunc);
            } else if (id == IDC_CANCEL_BTN) {
                if (!g_busy.load()) break;
                g_cancel = true;
                SetLed(RGB(180, 130, 30), L"取消清理");
                AppendLog(L"已取消, 清理分片残留...");
            } else if (id == IDC_BROWSE_BTN) {
                wchar_t path[MAX_PATH] = {};
                std::wstring cur = GetText(IDC_PATH_EDIT);
                if (!cur.empty()) wcsncpy(path, cur.c_str(), MAX_PATH - 1);
                else {
                    std::wstring url = GetText(IDC_URL_EDIT);
                    url = url.substr(0, url.find_first_of(L"\r\n"));
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
                if (GetSaveFileNameW(&ofn)) SetText(IDC_PATH_EDIT, path);
            }
            return 0;
        }

        case WM_APP_PROGRESS: {
            std::int64_t done = g_done.load(), total = g_total.load(), speed = g_speed.load();
            wchar_t buf[128];
            if (total > 0) {
                double pct = done * 100.0 / total;
                SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS), PBM_SETPOS, (WPARAM)(pct * 10), 0);
                swprintf(buf, 128, L"%s / %s", FmtBytes((double)done).c_str(), FmtBytes((double)total).c_str());
            } else {
                SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS), PBM_SETPOS, (WPARAM)std::min<std::int64_t>(1000, done / 102400), 0);
                swprintf(buf, 128, L"%s", FmtBytes((double)done).c_str());
            }
            SetText(IDC_M_DONE, buf);
            swprintf(buf, 128, L"%.2f MB/s", speed / 1048576.0);
            SetText(IDC_M_SPEED, buf);
            if (total > 0 && speed > 0) {
                swprintf(buf, 128, L"约 %lld 秒", (long long)((total - done) / speed));
            } else {
                swprintf(buf, 128, L"—");
            }
            SetText(IDC_M_ETA, buf);
            return 0;
        }

        case WM_APP_ERROR:
            SetLed(RGB(200, 60, 60), L"失败");
            AppendLog(L"错误: " + Utf8ToWide(g_error));
            SetBusy(false);
            return 0;

        case WM_APP_DONE: {
            bool ok = (wp == 1);
            SetBusy(false);
            if (ok) {
                SetLed(RGB(20, 140, 90), L"完成");
                AppendLog(L"完成: " + Utf8ToWide(g_path));
            } else if (!g_cancel.load()) {
                SetLed(RGB(200, 60, 60), L"失败");
            } else {
                SetLed(RGB(140, 144, 150), L"就绪");
            }
            if (g_thread.joinable()) g_thread.join();
            return 0;
        }

        case WM_CLOSE:
            if (g_busy.load()) {
                if (MessageBoxW(hwnd, L"下载正在进行, 确定退出? (将取消下载并清理分片)",
                                L"AutoSlice", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                g_cancel = true;
                if (g_thread.joinable()) g_thread.join();
            }
            DestroyWindow(hwnd);
            return 0;

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
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"AutoSliceWindow", L"AutoSlice-download",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 760, 640,
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
