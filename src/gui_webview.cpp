// gui_webview.cpp - AutoSlice-download WebView2 版 GUI
// Win32 宿主窗口 + WebView2 加载 prototype/index.html
// JS <-> C++ 通过 chrome.webview.postMessage 双向通信; 引擎复用 downloader.cpp
// 依赖: WebView2Loader.dll 需与 exe 同目录 (third_party/webview2/build/native/x64/)
#include "downloader.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

// WebView2 C++ 头 (MinGW 可直接编译)
#include "WebView2.h"

// ===== WebView2Loader.dll 动态加载 =====
// 注: 3 参版 CreateCoreWebView2EnvironmentWithOptions 在本机调用崩溃 (Loader C 回调问题),
//     改用 2 参版 CreateCoreWebView2Environment (同样创建默认 Runtime 环境)
typedef HRESULT(WINAPI* FnCreateWebView2Env)(
    PCWSTR browserExecutableFolder,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);

// ===== 全局状态 =====
static HWND g_hwnd = nullptr;
static ICoreWebView2* g_wv = nullptr;
static ICoreWebView2Controller* g_ctrl = nullptr;
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_cancel{false};
static std::thread g_thread;
static FnCreateWebView2Env g_createEnv = nullptr;

static std::vector<std::string> g_urls;
static std::string g_path;
static pcl_dl::DownloadOptions g_opt;
static std::string g_error;
static std::atomic<std::int64_t> g_done{0}, g_total{0}, g_speed{0}, g_last_post_ms{0};

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_ERROR    (WM_APP + 2)
#define WM_APP_DONE     (WM_APP + 3)
#define WM_APP_INIT     (WM_APP + 4)

// 运行时定位 WebView2 Runtime 目录 (不硬编码安装路径/版本号)
// 枚举两个标准安装位置下 Application 的版本子目录, 取版本号最大者
static std::wstring FindWebView2RuntimeDir() {
    const wchar_t* roots[] = {
        L"C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application",
        L"C:\\Program Files\\Microsoft\\EdgeWebView\\Application",
    };
    auto ParseVersion = [](const wchar_t* s, unsigned long out[4]) -> bool {
        int seg = 0; unsigned long cur = 0; bool any = false;
        for (const wchar_t* p = s; *p && seg < 4; ++p) {
            if (*p == L'.') { out[seg++] = cur; cur = 0; any = false; continue; }
            if (*p < L'0' || *p > L'9') return false;
            cur = cur * 10 + (*p - L'0');
            any = true;
        }
        if (!any && seg == 0) return false;
        if (seg < 4) { out[seg++] = cur; }
        while (seg < 4) out[seg++] = 0;
        return true;
    };
    auto Newer = [](const unsigned long a[4], const unsigned long b[4]) -> bool {
        for (int i = 0; i < 4; ++i) {
            if (a[i] != b[i]) return a[i] > b[i];
        }
        return false;
    };
    std::wstring best;
    unsigned long bestVer[4] = {0, 0, 0, 0};
    for (const wchar_t* root : roots) {
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((std::wstring(root) + L"\\*").c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            unsigned long v[4];
            if (!ParseVersion(fd.cFileName, v)) continue;
            // 该版本目录下必须存在浏览器可执行文件
            if (GetFileAttributesW((std::wstring(root) + L"\\" + fd.cFileName + L"\\msedgewebview2.exe").c_str())
                    == INVALID_FILE_ATTRIBUTES) continue;
            if (best.empty() || Newer(v, bestVer)) {
                best = std::wstring(root) + L"\\" + fd.cFileName;
                for (int i = 0; i < 4; ++i) bestVer[i] = v[i];
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return best;
}

// ===== 工具 =====
static std::wstring g_exeDir;
static void DebugLog(const wchar_t* tag, const std::wstring& msg) {
    FILE* f = _wfopen((g_exeDir + L"\\wv_debug.log").c_str(), L"ab");
    if (f) { fwprintf(f, L"[%ls] %ls\n", tag, msg.c_str()); fclose(f); }
}
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
// JSON 转义 (值中的 " \ 换行)
static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'"': o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n"; break;
            case L'\r': break;
            default: o += c;
        }
    }
    return o;
}
// 提取 JSON 字段 "key":"value" (value 会反转义)
static std::wstring GetJsonField(const std::wstring& json, const std::wstring& key) {
    std::wstring pat = L"\"" + key + L"\":\"";
    size_t p = json.find(pat);
    if (p == std::wstring::npos) return L"";
    p += pat.size();
    std::wstring out;
    for (; p < json.size(); ++p) {
        wchar_t c = json[p];
        if (c == L'\\' && p + 1 < json.size()) {
            wchar_t n = json[++p];
            if (n == L'n') out += L'\n';
            else if (n == L'r') out += L'\r';
            else if (n == L't') out += L'\t';
            else if (n == L'u') { /* \uXXXX 简略: 取 4 位 hex 转 wchar */ }
            else out += n;
        } else if (c == L'"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}
// 向 JS 发送消息 (C++ -> JS)
static void SendToJs(const std::wstring& json) {
    if (g_wv) g_wv->PostWebMessageAsJson(json.c_str());
}
static void JsMsg(const wchar_t* type, const std::wstring& payloadJson) {
    SendToJs(L"{\"type\":\"" + std::wstring(type) + L"\"," + payloadJson + L"}");
}
static void JsState(const wchar_t* phase, const wchar_t* text) {
    JsMsg(L"state", L"\"phase\":\"" + std::wstring(phase) + L"\",\"text\":\"" + std::wstring(text) + L"\"");
}
static void JsLog(const std::wstring& text, const wchar_t* cls) {
    JsMsg(L"log", L"\"text\":\"" + JsonEscape(text) + L"\",\"cls\":\"" + std::wstring(cls) + L"\"");
}
static void JsBusy(bool busy) {
    JsMsg(L"busy", busy ? L"\"busy\":true" : L"\"busy\":false");
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

// ===== 浏览: 保存对话框 (C++ 原生) =====
static void DoBrowse(HWND hwnd) {
    wchar_t path[MAX_PATH] = {};
    std::wstring cur = Utf8ToWide(g_path);
    if (!cur.empty()) {
        wcsncpy(path, cur.c_str(), MAX_PATH - 1);
    } else if (!g_urls.empty()) {
        std::string u = g_urls[0];
        auto q = u.find_first_of("?#");
        if (q != std::string::npos) u = u.substr(0, q);
        auto slash = u.find_last_of('/');
        if (slash != std::string::npos) u = u.substr(slash + 1);
        std::wstring fn = Utf8ToWide(u.empty() ? "download.bin" : u);
        wcsncpy(path, fn.c_str(), MAX_PATH - 1);
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
        g_path = WideToUtf8(path);
        JsMsg(L"path", L"\"path\":\"" + JsonEscape(std::wstring(path)) + L"\"");
    }
}

// ===== WebView2 回调 =====
class EnvCreatedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        DebugLog(L"env", std::to_wstring(result));
        if (FAILED(result) || !env) {
            MessageBoxW(g_hwnd, L"WebView2 环境创建失败（需要安装 WebView2 Runtime / Edge）",
                        L"AutoSlice", MB_OK | MB_ICONERROR);
            return S_OK;
        }
        // 创建控制器绑定到窗口
        struct CtrlHandler : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
            ULONG STDMETHODCALLTYPE Release() override { return 1; }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
            HRESULT STDMETHODCALLTYPE Invoke(HRESULT r2, ICoreWebView2Controller* controller) override {
                DebugLog(L"ctrl", std::to_wstring(r2));
                if (FAILED(r2) || !controller) return S_OK;
                g_ctrl = controller;
                HRESULT hr = controller->get_CoreWebView2(&g_wv);
                DebugLog(L"getwv", std::to_wstring(hr));
                controller->put_IsVisible(TRUE);
                RECT rc; GetClientRect(g_hwnd, &rc);
                controller->put_Bounds(rc);

                // 注册 JS 消息接收
                struct MsgHandler : ICoreWebView2WebMessageReceivedEventHandler {
                    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
                    ULONG STDMETHODCALLTYPE Release() override { return 1; }
                    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
                    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* wv, ICoreWebView2WebMessageReceivedEventArgs* args) override {
                        LPWSTR json = nullptr;
                        args->get_WebMessageAsJson(&json);
                        if (!json) return S_OK;
                        std::wstring msg(json);
                        CoTaskMemFree(json);
                        std::wstring cmd = GetJsonField(msg, L"cmd");
                        if (cmd == L"start") {
                            if (g_busy.load()) return S_OK;
                            // 解析表单
                            std::wstring urlsText = GetJsonField(msg, L"urls");
                            g_urls.clear();
                            size_t pos = 0;
                            while (pos <= urlsText.size()) {
                                size_t eol = urlsText.find_first_of(L"\r\n", pos);
                                std::wstring line = urlsText.substr(pos, (eol == std::wstring::npos ? urlsText.size() : eol) - pos);
                                if (!line.empty()) g_urls.push_back(WideToUtf8(line));
                                if (eol == std::wstring::npos) break;
                                pos = eol + 1;
                            }
                            g_path = WideToUtf8(GetJsonField(msg, L"path"));
                            if (g_path.empty() && !g_urls.empty()) {
                                std::string u = g_urls[0];
                                auto q = u.find_first_of("?#");
                                if (q != std::string::npos) u = u.substr(0, q);
                                auto slash = u.find_last_of('/');
                                if (slash != std::string::npos) u = u.substr(slash + 1);
                                g_path = u.empty() ? "download.bin" : u;
                            }
                            pcl_dl::DownloadOptions opt;
                            opt.max_threads = _wtoi(GetJsonField(msg, L"threads").c_str());
                            opt.speed_cap = (std::int64_t)_wtoi(GetJsonField(msg, L"speedCap").c_str()) * 1024;
                            if (opt.speed_cap < 0) opt.speed_cap = -1;
                            opt.max_retries = std::max(1, _wtoi(GetJsonField(msg, L"retry").c_str()));
                            opt.retry_delay_ms = std::max(0, _wtoi(GetJsonField(msg, L"delay").c_str()));
                            g_opt = opt;
                            g_cancel = false;
                            g_error.clear();
                            g_done = g_total = g_speed = 0;
                            JsBusy(true);
                            JsState(L"busy", L"下载中");
                            JsLog(L"开始下载: " + Utf8ToWide(g_path), L"info");
                            for (auto& u : g_urls) JsLog(L"  源: " + Utf8ToWide(u), L"info");
                            JsLog(L"探测文件大小...", L"info");
                            g_busy = true;
                            g_thread = std::thread(DownloadThreadFunc);
                        } else if (cmd == L"browse") {
                            DoBrowse(g_hwnd);
                        } else if (cmd == L"cancel") {
                            g_cancel = true;
                            JsState(L"cancel", L"取消清理");
                            JsLog(L"已取消, 清理分片残留...", L"info");
                        }
                        return S_OK;
                    }
                };
                static MsgHandler msgHandler;
                EventRegistrationToken token{};
                g_wv->add_WebMessageReceived(&msgHandler, &token);

                // 导航到本地原型: 同目录优先, 回退上级目录 (不依赖固定目录结构)
                wchar_t path[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, path, MAX_PATH);
                std::wstring dir(path);
                auto slash = dir.find_last_of(L'\\');
                dir = dir.substr(0, slash);
                std::wstring uri;
                const wchar_t* cands[] = {
                    L"\\prototype\\index.html",      // 发布布局: exe 与 prototype 同目录
                    L"\\..\\prototype\\index.html",  // 开发布局: exe 在 build/ 下
                };
                for (auto c : cands) {
                    std::wstring p = dir + c;
                    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        std::wstring fw;
                        for (wchar_t ch : p) fw += (ch == L'\\') ? L'/' : ch;  // file URI 用正斜杠
                        uri = L"file:///" + fw;
                        break;
                    }
                }
                if (uri.empty()) {
                    MessageBoxW(g_hwnd, L"未找到 prototype/index.html（请将原型文件放在 exe 同目录）",
                                L"AutoSlice", MB_OK | MB_ICONERROR);
                    return S_OK;
                }
                DebugLog(L"nav", uri);
                g_wv->Navigate(uri.c_str());
                return S_OK;
            }
        };
        static CtrlHandler ctrlHandler;
        env->CreateCoreWebView2Controller(g_hwnd, &ctrlHandler);
        return S_OK;
    }
};

// ===== 窗口过程 =====
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            DebugLog(L"wmain", L"WM_CREATE");
            // 动态加载 WebView2Loader.dll (与 exe 同目录)
            g_createEnv = (FnCreateWebView2Env)GetProcAddress(
                LoadLibraryW(L"WebView2Loader.dll"), "CreateCoreWebView2Environment");
            if (!g_createEnv) {
                MessageBoxW(hwnd, L"未找到 WebView2Loader.dll（请将其放在程序目录）",
                            L"AutoSlice", MB_OK | MB_ICONERROR);
                return -1;
            }
            DebugLog(L"wmain", L"loader ok");
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            {
                static EnvCreatedHandler envHandler;
                // 运行时定位 WebView2 Runtime 目录: 枚举两个安装位置的版本目录, 取最新
                std::wstring wvDir = FindWebView2RuntimeDir();
                HRESULT hr = g_createEnv(wvDir.empty() ? nullptr : wvDir.c_str(), &envHandler);
                DebugLog(L"wmain", L"create env: " + std::to_wstring(hr) + L" dir=" + (wvDir.empty() ? L"<默认>" : wvDir));
            }
            return 0;

        case WM_SIZE:
            if (g_ctrl) {
                RECT rc; GetClientRect(hwnd, &rc);
                g_ctrl->put_Bounds(rc);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = 480;
            mmi->ptMinTrackSize.y = 520;
            return 0;
        }

        case WM_APP_PROGRESS: {
            wchar_t buf[512];
            std::int64_t done = g_done.load(), total = g_total.load(), speed = g_speed.load();
            swprintf(buf, 512, L"\"done\":%lld,\"total\":%lld,\"speed\":%lld", done, total, speed);
            JsMsg(L"progress", buf);
            return 0;
        }

        case WM_APP_ERROR:
            JsState(L"err", L"失败");
            JsLog(L"错误: " + Utf8ToWide(g_error), L"err");
            JsBusy(false);
            return 0;

        case WM_APP_DONE: {
            bool ok = (wp == 1);
            JsBusy(false);
            if (ok) {
                JsState(L"ok", L"完成");
                JsLog(L"完成: " + Utf8ToWide(g_path), L"ok");
            } else if (!g_cancel.load()) {
                JsState(L"err", L"失败");
                JsLog(L"失败（详情见上方错误）", L"err");
            } else {
                JsState(L"idle", L"就绪");
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
            if (g_wv) { g_wv->Release(); g_wv = nullptr; }
            if (g_ctrl) { g_ctrl->Release(); g_ctrl = nullptr; }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    wchar_t pm[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, pm, MAX_PATH);
    g_exeDir = pm;
    auto sl = g_exeDir.find_last_of(L'\\');
    g_exeDir = g_exeDir.substr(0, sl);
    DebugLog(L"boot", g_exeDir);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"AutoSliceWebView";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, L"AutoSliceWebView", L"AutoSlice-download",
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
