// downloader.cpp - PCL 风格多线程分片下载引擎实现
#include "downloader.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace pcl_dl {

// ===================== 工具 =====================

std::string WsToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWs(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string GetLastErrorStr() {
    DWORD err = GetLastError();
    if (err == 0) return "no error";
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::string out = buf ? WsToUtf8(buf) : std::to_string(err);
    if (buf) LocalFree(buf);
    return out;
}

// ===================== DownloadTask =====================

DownloadTask::DownloadTask(std::vector<std::string> urls, std::string local_path)
    : local_path(std::move(local_path)) {
    std::sort(urls.begin(), urls.end());
    urls.erase(std::unique(urls.begin(), urls.end()), urls.end());
    int id = 0;
    for (auto& u : urls) {
        auto s = std::make_unique<Source>();
        s->url = u;
        s->id = id++;
        s->failed = false;
        sources.push_back(std::move(s));
    }
}

// 解析 URL 中的 host / path / port / use_ssl
static bool ParseUrl(const std::string& url, std::wstring* host, std::wstring* path,
                     INTERNET_PORT* port, bool* use_ssl) {
    std::wstring wurl = Utf8ToWs(url);
    URL_COMPONENTSW comp{};
    comp.dwStructSize = sizeof(comp);
    comp.dwHostNameLength = (DWORD)-1;
    comp.dwUrlPathLength = (DWORD)-1;
    comp.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &comp))
        return false;
    *host = std::wstring(comp.lpszHostName, comp.dwHostNameLength);
    *port = comp.nPort;
    *use_ssl = (comp.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring wp = comp.lpszUrlPath ? std::wstring(comp.lpszUrlPath, comp.dwUrlPathLength) : L"/";
    std::wstring we = comp.lpszExtraInfo ? std::wstring(comp.lpszExtraInfo, comp.dwExtraInfoLength) : L"";
    *path = wp + we;
    return true;
}

bool HttpDownloadRange(const std::string& url,
                       std::int64_t start, std::int64_t end,
                       const std::function<bool(const char*, size_t)>& on_data,
                       std::string* err_out,
                       std::int64_t speed_cap,
                       std::atomic<std::int64_t>& token_left) {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool use_ssl = false;
    if (!ParseUrl(url, &host, &path, &port, &use_ssl)) {
        if (err_out) *err_out = "解析 URL 失败: " + url;
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"PCL-Downloader/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) {
        if (err_out) *err_out = "WinHttpOpen: " + GetLastErrorStr();
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        if (err_out) *err_out = "WinHttpConnect: " + GetLastErrorStr();
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (use_ssl) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        if (err_out) *err_out = "WinHttpOpenRequest: " + GetLastErrorStr();
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Range 头
    std::wstring range_hdr;
    if (start >= 0) {
        wchar_t buf[64];
        swprintf(buf, 64, L"Range: bytes=%lld-%lld\r\n", (long long)start, (long long)end);
        range_hdr = buf;
    }

    if (!WinHttpSendRequest(hReq, range_hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range_hdr.c_str(),
                            range_hdr.empty() ? 0 : (DWORD)range_hdr.size(),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (err_out) *err_out = "WinHttpSendRequest: " + GetLastErrorStr();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        if (err_out) *err_out = "WinHttpReceiveResponse: " + GetLastErrorStr();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // 检查状态码
    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200 && status != 206) {
        char buf[128];
        snprintf(buf, sizeof(buf), "HTTP %lu", (unsigned long)status);
        if (err_out) *err_out = buf;
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // 设置较短的超时, 使 Ctrl+C 取消时阻塞的 WinHttpReadData 能较快返回
    DWORD timeout_ms = 5000;
    WinHttpSetTimeouts(hReq, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    // 读取数据
    char buf[64 * 1024];
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf, sizeof(buf), &read)) {
            if (err_out) *err_out = "WinHttpReadData: " + GetLastErrorStr();
            ok = false;
            break;
        }
        if (read == 0) break;
        if (on_data) {
            if (!on_data(buf, read)) {
                // 提前停止 (分片已被切分, 本线程边界已到) — 视为成功
                break;
            }
        }
        // 限速令牌
        if (speed_cap > 0) {
            token_left -= (std::int64_t)read;
            while (token_left.load() < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// 探测: 发 Range: bytes=0-0 请求, 从 Content-Range 解析总大小
// 依次尝试所有源, 第一个成功的为准 (对应 PCL: 源失败自动切换)
bool DownloadTask::ProbeWithRange(std::int64_t* size_out, bool* supports_range) {
    std::vector<std::string> fail_reasons;  // 每个源失败的具体原因
    for (auto& src : sources) {
        if (src->failed.load()) {
            fail_reasons.push_back(src->url + " (已标记失败)");
            continue;
        }
        std::int64_t total = -1;
        bool range_ok = false;
        bool got = false;
        std::string reason = "未知错误";
        std::wstring host, path;
        INTERNET_PORT port = 0;
        bool use_ssl = false;
        if (!ParseUrl(src->url, &host, &path, &port, &use_ssl)) {
            src->failed = true;
            fail_reasons.push_back(src->url + " (URL 解析失败)");
            continue;
        }
        HINTERNET hSession = WinHttpOpen(L"PCL-Downloader/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!hSession) {
            src->failed = true;
            fail_reasons.push_back(src->url + " (WinHttpOpen: " + GetLastErrorStr() + ")");
            continue;
        }
        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConnect) {
            reason = "WinHttpConnect: " + GetLastErrorStr();
            WinHttpCloseHandle(hSession);
            src->failed = true;
            fail_reasons.push_back(src->url + " (" + reason + ")");
            continue;
        }
        DWORD flags = WINHTTP_FLAG_REFRESH | (use_ssl ? WINHTTP_FLAG_SECURE : 0);
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) {
            reason = "WinHttpOpenRequest: " + GetLastErrorStr();
            WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            src->failed = true;
            fail_reasons.push_back(src->url + " (" + reason + ")");
            continue;
        }
        LPCWSTR hdr = L"Range: bytes=0-0\r\n";
        bool sent = WinHttpSendRequest(hReq, hdr, (DWORD)wcslen(hdr), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0;
        if (sent) sent = WinHttpReceiveResponse(hReq, nullptr) != 0;
        if (!sent) {
            reason = "请求失败: " + GetLastErrorStr();
        } else {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (status == 206) {
                // Content-Range: bytes 0-0/总大小
                wchar_t cr[128] = {};
                DWORD crsz = sizeof(cr);
                if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
                                        cr, &crsz, WINHTTP_NO_HEADER_INDEX)) {
                    std::wstring ws(cr);
                    auto pos = ws.find(L'/');
                    if (pos != std::wstring::npos) {
                        total = _wtoi64(ws.substr(pos + 1).c_str());
                        range_ok = total > 0;
                    }
                }
                if (total <= 0) reason = "Content-Range 解析失败";
            } else if (status == 200) {
                // 服务器忽略 Range: 无 Content-Range, 直接看 Content-Length
                wchar_t cl[64] = {};
                DWORD clsz = sizeof(cl);
                if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                        cl, &clsz, WINHTTP_NO_HEADER_INDEX)) {
                    total = _wtoi64(cl);
                }
                if (total <= 0) {
                    // 无 Content-Length → chunked/流式响应 (如 GitHub codeload、大文件流)。
                    // 大小未知但连接正常, 标记 unknown-size, 走单线程无分片下载。
                    reason = "未知大小 (chunked/流式响应)";
                    got = true;   // 视为"探测成功-未知大小"
                    total = -1;   // 标记: -1 = 未知大小
                } else {
                    range_ok = false;  // 不支持分片 (但大小已知)
                }
            } else {
                reason = "HTTP " + std::to_string(status);
            }
            got = total > 0 || (total == -1 && got);
        }
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (got) {
            *size_out = total;
            *supports_range = range_ok;
            return true;
        }
        src->failed = true;  // 该源探测失败, 标记后试下一个
        fail_reasons.push_back(src->url + " (" + reason + ")");
    }
    error_ = "所有源探测文件大小均失败:\n";
    for (const auto& r : fail_reasons) error_ += "  " + r + "\n";
    return false;
}

bool DownloadTask::ProbeSize() {
    std::int64_t size = -1;
    bool range_ok = false;
    if (!ProbeWithRange(&size, &range_ok)) return false;
    file_size_ = size;
    // size = -1: 未知大小 (chunked) → 单线程, 不分片
    no_split_ = (size < 0) || !range_ok || size < 1024 * 1024;
    return true;
}

std::int64_t DownloadTask::PieceEndLocked(const Piece& p) const {
    // 链表下一个节点的 start - 1
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (pieces[i].get() == &p) {
            if (i + 1 < pieces.size()) return pieces[i + 1]->start - 1;
            break;
        }
    }
    // 未知大小 (chunked): 返回一个极大值表示"读到流结束为止"
    if (file_size_ < 0) return 1000LL * 1000 * 1000 * 1000;  // 约 1TB
    return file_size_ - 1;
}

std::int64_t DownloadTask::PieceEnd(const Piece& p) const {
    std::lock_guard<std::mutex> lk(chain_mtx);
    return PieceEndLocked(p);
}

Source* DownloadTask::GetSource(std::int64_t from_id) {
    std::lock_guard<std::mutex> lk(src_mtx);
    if (sources.empty()) return nullptr;
    std::int64_t id = from_id;
    if (id >= (std::int64_t)sources.size() || id < 0) id = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        Source* s = sources[(id + i) % sources.size()].get();
        if (!s->failed.load()) return s;
    }
    return nullptr;
}

bool DownloadTask::HasAvailableSource() const {
    std::lock_guard<std::mutex> lk(src_mtx);
    for (auto& s : sources)
        if (!s->failed.load()) return true;
    return false;
}

std::int64_t DownloadTask::TotalDone() const {
    std::int64_t sum = 0;
    std::lock_guard<std::mutex> lk(chain_mtx);
    for (auto& p : pieces) sum += p->done.load();
    return sum;
}

bool DownloadTask::AllFinished() const {
    std::lock_guard<std::mutex> lk(chain_mtx);
    if (pieces.empty()) return false;
    for (auto& p : pieces)
        if (!p->finished.load()) return false;
    return true;
}

bool DownloadTask::MergeFiles() {
    // 未知大小 (chunked): 只有单分片, part 文件就是完整内容, 直接改名
    if (IsUnknownSize()) {
        std::lock_guard<std::mutex> lk(chain_mtx);
        if (pieces.size() != 1) { error_ = "未知大小文件分片数异常: " + std::to_string(pieces.size()); return false; }
        Piece& p = *pieces[0];
        std::error_code ec;
        fs::rename(p.part_path, local_path, ec);
        if (ec) {
            // rename 失败 (跨卷等) → 复制后删除
            std::error_code ec2;
            fs::copy_file(p.part_path, local_path, ec2);
            if (ec2) { error_ = "合并分片失败: " + local_path + " (" + ec2.message() + ")"; return false; }
            fs::remove(p.part_path, ec2);
        }
        return true;
    }

    // 按最终链表边界精确截取每个分片: 每个分片文件覆盖 [start, end]
    // (切分竞态下旧线程文件可能包含越过新边界的尾部, 这里只取前 (end-start+1) 字节, 丢弃越界数据)
    FILE* out = nullptr;
    if (fopen_s(&out, local_path.c_str(), "wb") != 0 || !out) {
        error_ = "无法创建目标文件: " + local_path;
        return false;
    }
    bool ok = true;
    char buf[64 * 1024];
    {
        std::lock_guard<std::mutex> lk(chain_mtx);
        for (size_t i = 0; i < pieces.size(); ++i) {
            Piece& p = *pieces[i];
            std::int64_t end = (i + 1 < pieces.size()) ? pieces[i + 1]->start - 1 : file_size_ - 1;
            std::int64_t expect = end - p.start + 1;   // 该分片应有的字节数
            if (p.done.load() < expect) { ok = false; break; }  // 分片数据不足
            FILE* in = nullptr;
            if (fopen_s(&in, p.part_path.c_str(), "rb") != 0 || !in) { ok = false; break; }
            // 调试: 校验分片文件大小
            fseek(in, 0, SEEK_END);
            std::int64_t file_sz = ftell(in);
            fseek(in, 0, SEEK_SET);
            if (file_sz < expect) { ok = false; fclose(in); break; }  // 文件不足
            std::int64_t written = 0;
            while (written < expect) {
                size_t want = (size_t)std::min<std::int64_t>((std::int64_t)sizeof(buf), expect - written);
                size_t n = fread(buf, 1, want, in);
                if (n == 0) { ok = false; break; }
                fwrite(buf, 1, n, out);
                written += (std::int64_t)n;
            }
            fclose(in);
            fs::remove(p.part_path);
            if (!ok) break;
        }
    }
    fclose(out);
    if (!ok) { error_ = "合并分片失败: " + local_path; return false; }
    return true;
}

// ===================== DownloadEngine =====================

void DownloadEngine::Worker(Ctx ctx) {
    DownloadTask& task = *ctx.task;
    Piece& piece = *ctx.piece;
    DownloadEngine& engine = *ctx.engine;

    const std::int64_t thread_start_ms = GetTickCount64();
    std::atomic<bool> first_data{false};  // 首个数据块已到达 → 连接完成

    // 记录该线程上一次用的源; 失败后从下一个源开始重试 (对应 PCL GetSource(Thread.Source.Id + 1))
    std::int64_t source_id = 0;
    {
        std::lock_guard<std::mutex> lk(task.src_mtx);
        for (size_t i = 0; i < task.sources.size(); ++i)
            if (!task.sources[i]->failed.load()) { source_id = i; break; }
    }

    while (!piece.finished.load() && !task.task_failed_.load()) {
        // 外部取消 (Ctrl+C): 立即停止, 让失败清理路径删除分片
        if (engine.opt.cancel_flag && engine.opt.cancel_flag->load()) {
            task.task_failed_ = true;
            task.error_ = "已取消";
            break;
        }
        Source* src = task.GetSource(source_id);
        if (!src) {
            piece.failed = true;
            task.task_failed_ = true;
            task.error_ = "所有下载源均失败: " + task.local_path;
            break;
        }

        // 分片临时文件 (追加模式, 断点续传)
        FILE* part = nullptr;
        if (fopen_s(&part, piece.part_path.c_str(), "ab") != 0 || !part) {
            piece.failed = true;
            task.task_failed_ = true;
            task.error_ = "无法创建分片文件: " + piece.part_path;
            break;
        }

        // 实时边界: 每次读取前重算 PieceEnd, 若已被切分则本线程只下到新边界
        // (对应 PCL: While Th.DownloadUndone > 0 ... Math.Min(HttpDataCount, Th.DownloadUndone))
        std::string err;
        bool unknown_size = task.IsUnknownSize();
        // 未知大小 (chunked): 不带 Range, 全量读取到 EOF; 已知大小: 带 Range 分片
        std::int64_t req_start = unknown_size ? -1 : piece.start + piece.done.load();
        bool ok = HttpDownloadRange(
            src->url, req_start, task.PieceEnd(piece),
            [&](const char* data, size_t len) -> bool {
                if (!first_data.exchange(true)) {
                    // 首个数据块: 记录连接耗时 (线程创建 → 首字节)
                    engine.connect_total_ms.fetch_add(GetTickCount64() - thread_start_ms);
                    engine.connect_count.fetch_add(1);
                }
                std::int64_t end = task.PieceEnd(piece);
                std::int64_t pos = piece.start + piece.done.load();
                // 校验: 文件当前大小必须等于已写入字节数 (防止多 worker 写同一文件)
                long cur = ftell(part);
                if (cur != piece.done.load()) {
                    if (!engine.opt.quiet) {
                        printf("  [BUG] %s: ftell=%ld done=%lld 文件被其他写入者占用!\n",
                               piece.part_path.c_str(), cur, (long long)piece.done.load());
                    }
                    piece.failed = true;
                    task.task_failed_ = true;
                    task.error_ = "分片文件写入位置冲突: " + piece.part_path;
                    return false;
                }
                if (!unknown_size && pos > end) return false;  // 已知大小: 到边界停止
                std::int64_t allowed = end - pos + 1;
                size_t wlen = unknown_size ? len
                            : (size_t)std::min<std::int64_t>(allowed, (std::int64_t)len);
                fwrite(data, 1, wlen, part);
                piece.done.fetch_add((std::int64_t)wlen);
                return unknown_size || pos + (std::int64_t)wlen <= end;  // 未知大小: 读到 EOF
            },
            &err, engine.opt.speed_cap, engine.tokens);

        fclose(part);

        if (ok) {
            if (unknown_size) {
                // 未知大小: 连接正常读到 EOF 即完成 (失败换源由 else 分支处理)
                piece.finished = true;
                break;
            }
            // 检查是否真正下完了自己的区间 (切分后边界收缩, 需要继续循环)
            std::int64_t end = task.PieceEnd(piece);
            if (piece.start + piece.done.load() > end) {
                piece.finished = true;
                break;
            }
            // 未下完但连接正常结束 → 服务器提前断流, 换源重试
            src->failed = true;
            source_id = src->id + 1;
        } else {
            // 该源失败 → 换下一个源, 从断点继续
            src->failed = true;
            source_id = src->id + 1;
        }
    }
    engine.thread_count.fetch_sub(1);
}

bool DownloadEngine::TryBeginThread(DownloadTask& task) {
    // 1. 条件检测: 全局线程数上限 (0 = 自动, 硬上限 32) / 可用源
    int limit = opt.max_threads;
    if (limit <= 0) limit = 32;
    if (thread_count.load() >= limit) return false;
    if (!task.HasAvailableSource()) return false;
    // 未知大小 (chunked): 只能单线程全量下载, 不允许加线程/切分
    if (task.IsUnknownSize() && !task.pieces.empty()) return false;

    Piece* new_piece = nullptr;
    {
        std::lock_guard<std::mutex> lk(task.chain_mtx);

        // 2. 首线程: 从偏移 0 开始, 覆盖全文件 (之后动态切分)
        if (task.pieces.empty()) {
            auto p = std::make_unique<Piece>(0, task.local_path + ".part0");
            new_piece = p.get();
            task.pieces.push_back(std::move(p));
        } else {
            // 3. 动态切分: 找剩余量最大的碎片, 从剩余量的 40% 处切一刀
            //    对应 PCL: FilePieceMax.DownloadEnd - FilePieceMax.DownloadUndone * 0.4
            Piece* max_piece = nullptr;
            std::int64_t max_undone = 0;
            for (auto& p : task.pieces) {
                if (p->finished.load() || p->failed.load()) continue;
                std::int64_t end = task.PieceEndLocked(*p);
                std::int64_t undone = end - (p->start + p->done.load()) + 1;
                if (undone > max_undone) { max_undone = undone; max_piece = p.get(); }
            }
            if (!max_piece) return false;
            if (max_undone < opt.piece_limit) return false;  // 碎片 < 256KB 不再切

            std::int64_t split = max_piece->start + max_piece->done.load() + max_undone * opt.split_ratio_num / 100;
            std::int64_t cur_end = max_piece->start + max_piece->done.load();
            if (split <= cur_end) split = cur_end + 1;
            std::int64_t piece_end = task.PieceEndLocked(*max_piece);
            if (split >= piece_end) return false;

            auto p = std::make_unique<Piece>(split, task.local_path + ".part" + std::to_string(task.pieces.size()));
            new_piece = p.get();
            // 插入到 max_piece 之后 (链表有序)
            for (size_t i = 0; i < task.pieces.size(); ++i) {
                if (task.pieces[i].get() == max_piece) {
                    task.pieces.insert(task.pieces.begin() + i + 1, std::move(p));
                    break;
                }
            }
        }
    }

    // 4. 启动新线程
    thread_count.fetch_add(1);
    std::thread(Worker, Ctx{&task, new_piece, this}).detach();
    return true;
}

std::int64_t DownloadEngine::AvgConnectMs() const {
    int n = connect_count.load();
    if (n <= 0) return 0;
    return connect_total_ms.load() / n;
}

// 删除 <文件名>.part* 分片残留 (不动目标文件本身)
// worker 线程可能仍持有文件句柄 (WinHttpReadData 阻塞中), 删除失败时重试等待
void DownloadEngine::CleanupPartFiles(const std::string& local_path) {
    std::error_code ec;
    fs::path parent = fs::path(local_path).parent_path();
    std::string base = fs::path(local_path).filename().string();
    if (parent.empty()) parent = ".";
    if (!fs::exists(parent, ec)) return;

    // 收集目标分片文件
    std::vector<fs::path> targets;
    for (const auto& entry : fs::directory_iterator(parent)) {
        std::string name = entry.path().filename().string();
        if (name.rfind(base + ".part", 0) == 0) {
            targets.push_back(entry.path());
        }
    }
    // 删除, 文件被占用 (句柄未释放) 时重试最多 20 次, 每次 100ms
    for (int attempt = 0; attempt < 20; ++attempt) {
        bool all_gone = true;
        for (auto& p : targets) {
            std::error_code ec2;
            if (fs::exists(p, ec2)) {
                fs::remove(p, ec2);
                if (ec2) all_gone = false;  // 仍被占用, 稍后重试
            }
        }
        if (all_gone) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// 清理下载失败/中断后的残留: 删除所有 .part 分片文件和半成品输出文件
void DownloadEngine::CleanupFailedDownload(const std::string& local_path) {
    DownloadEngine::CleanupPartFiles(local_path);
    // 删除半成品输出文件 (下载失败时生成的)
    std::error_code ec;
    fs::remove(local_path, ec);
}

bool DownloadEngine::Download(std::vector<std::string> urls, const std::string& local_path,
                              const DownloadOptions& opt) {
    int retries = std::max(1, opt.max_retries);
    for (int attempt = 1; attempt <= retries; ++attempt) {
        // 返回值: 0=成功, 1=可重试失败(下载中断), 2=不可重试失败(探测失败, 如 404)
        int rc = DownloadOnce(urls, local_path, opt);
        if (rc == 0) return true;
        // 任何失败 (探测失败/下载中断): 都清理分片残留
        // 注意: 探测失败时目标文件可能是用户已有数据, 只清 .part 不删目标
        CleanupPartFiles(local_path);
        if (rc == 2) return false;  // 探测失败 (404/URL 错误): 重试无意义
        if (attempt < retries) {
            if (!opt.quiet) {
                printf("\n  [重试 %d/%d] %s 失败, %dms 后重新下载...\n",
                       attempt, retries - 1, local_path.c_str(), opt.retry_delay_ms);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.retry_delay_ms));
        }
    }
    return false;
}

int DownloadEngine::DownloadOnce(std::vector<std::string> urls, const std::string& local_path,
                                 const DownloadOptions& opt) {
    DownloadTask task(std::move(urls), local_path);

    // 1. 探测
    if (!task.ProbeSize()) {
        if (!opt.quiet) printf("  错误: %s\n", task.error_.c_str());
        // 探测失败分两类:
        //  - HTTP 4xx (URL 不存在等): 重试无意义 → 返回 2 不可重试
        //  - 其他 (5xx 服务器故障/网络错误/超时): 可能临时性故障 → 返回 1 可重试
        // 注意: 5xx 也属于临时故障, 应重试; 只有 4xx 是确定错误
        bool fatal = false;
        auto pos = task.error_.find("HTTP ");
        if (pos != std::string::npos) {
            int code = atoi(task.error_.c_str() + pos + 5);
            if (code >= 400 && code < 500) fatal = true;  // 4xx: 404/403 等确定错误
        }
        if (task.error_.find("URL 解析失败") != std::string::npos) fatal = true;
        return fatal ? 2 : 1;
    }

    // 1.5 清理残留分片文件 (上次异常退出的 .part* 会导致追加写入污染)
    CleanupFailedDownload(local_path);

    // 2. 启动调度线程 + 首个下载线程
    DownloadEngine engine;
    engine.opt = opt;
    engine.tokens = (opt.speed_cap > 0 ? opt.speed_cap : 0);  // 初始配额
    if (!engine.TryBeginThread(task)) {
        if (!opt.quiet) printf("  错误: %s\n", task.error_.empty() ? "无法启动下载线程" : task.error_.c_str());
        return 2;
    }

    // 3. 自适应调度循环: 每 poll_ms 检查一次
    //    对应 PCL FUTURE (ModNet.vb:856):
    //    "计算下载源平均链接时间和线程下载速度, 按最高时间节省来开启多线程"
    std::int64_t last_done = 0;
    std::int64_t last_ms = GetTickCount64();
    std::atomic<std::int64_t> speed{0};
    std::atomic<bool> finished{false};

    std::thread scheduler([&] {
        // 带宽饱和检测: 连续 N 次速度提升 < 阈值 → 判定带宽已达上限
        const int SATURATE_SAMPLES = 5;          // 连续采样次数
        const double IMPROVE_MIN = 1.08;         // 加线程后速度至少提升 8% 才继续加
        int improve_fail = 0;                    // 连续未达标次数
        std::int64_t prev_speed_at_add = 0;      // 上次加线程时的速度

        while (!task.AllFinished() && !task.HasFailed()) {
            // 外部取消 (Ctrl+C): 停止调度, 让任务走失败清理路径
            if (engine.opt.cancel_flag && engine.opt.cancel_flag->load()) {
                task.task_failed_ = true;
                task.error_ = "已取消";
                break;
            }
            // 限速令牌: 每 100ms 增加配额 (对应 PCL 限速器)
            if (engine.opt.speed_cap > 0) {
                std::int64_t cap = engine.opt.speed_cap / 10;
                if (cap > 0) engine.tokens.fetch_add(cap);
            } else if (engine.opt.speed_cap == 0) {
                // 自动限速: 按实测带宽上限的 80% 补充配额
                std::int64_t bw = engine.measured_bandwidth.load();
                if (bw > 0) {
                    std::int64_t cap = bw * 80 / 100 / 10;
                    if (cap > 0) engine.tokens.fetch_add(cap);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(engine.opt.poll_ms));
            // 计算速度
            std::int64_t now_done = task.TotalDone();
            std::int64_t now_ms = GetTickCount64();
            if (now_ms - last_ms >= 200) {
                std::int64_t dt = now_ms - last_ms;
                if (dt > 0) speed = (now_done - last_done) * 1000 / dt;
                last_done = now_done;
                last_ms = now_ms;
            }
            if (engine.opt.on_progress) engine.opt.on_progress(task.TotalDone(), task.file_size(), speed.load());

            // ---- 自适应加线程决策 ----
            int threads = engine.thread_count.load();
            int max_threads = engine.opt.max_threads;
            if (max_threads <= 0) max_threads = 32;  // 自动模式的硬上限

            if (threads <= 0) continue;  // 所有 worker 已退出 (任务收尾), 不再调度
            if (threads >= max_threads) continue;
            if (speed.load() <= 0) continue;  // 还没开始传数据, 稍等

            std::int64_t per_thread = speed.load() / threads;
            // 条件 1: 每线程速度过低 → 带宽饱和 (对应 PCL Speed >= NetTaskSpeedLimitLow)
            if (per_thread < engine.opt.per_thread_min) {
                engine.measured_bandwidth = speed.load();  // 记录实测带宽上限
                continue;
            }

            // 条件 2: 收益评估 — 加线程节省的时间是否大于连接开销
            //   节省时间 ≈ 剩余量/当前速度 - 剩余量/(当前速度+每线程速度)
            //   若节省时间 < 平均连接耗时 → 不值得加
            std::int64_t remaining = task.file_size() - task.TotalDone();
            if (remaining <= 0) continue;
            double cur_s = (double)speed.load();
            double est_saved = remaining / cur_s - remaining / (cur_s + per_thread);  // 秒
            double avg_conn = engine.AvgConnectMs() / 1000.0;
            if (est_saved < avg_conn) {
                // 连接开销大于收益, 不加
                continue;
            }

            // 条件 3: 加线程后速度无明显提升 → 饱和 (对应 PCL: 速度足够无需新增)
            if (prev_speed_at_add > 0) {
                double gain = (double)speed.load() / (double)prev_speed_at_add;
                if (gain < IMPROVE_MIN) {
                    improve_fail++;
                    if (improve_fail >= SATURATE_SAMPLES) {
                        engine.measured_bandwidth = speed.load();  // 记录实测带宽上限
                        continue;
                    }
                } else {
                    improve_fail = 0;
                }
            }

            // 条件 4: 碎片过小则 TryBeginThread 内部会拒绝, 这里直接尝试
            if (engine.TryBeginThread(task)) {
                prev_speed_at_add = speed.load();
                if (!engine.opt.quiet) {
                    printf("  [自适应] 加线程 → %d (每线程 %.0f KB/s, 预计节省 %.1fs, 连接 %.0fms)\n",
                           threads + 1, per_thread / 1024.0, est_saved, (double)engine.AvgConnectMs());
                }
            }
        }
        finished = true;
    });

    // 4. 等待完成
    while (!finished.load() && !task.HasFailed()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    scheduler.join();

    if (task.HasFailed()) {
        if (!opt.quiet) printf("  错误: %s\n", task.error_.c_str());
        if (opt.on_progress) opt.on_progress(task.TotalDone(), task.file_size(), speed.load());
        // 等所有 worker 线程退出并释放文件句柄, 再清理分片 (否则删除会因占用失败)
        for (int i = 0; i < 100 && engine.thread_count.load() > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CleanupFailedDownload(local_path);  // 失败自动清理分片和半成品
        return 1;  // 下载中断: 可重试
    }

    // 5. 合并分片
    if (!task.MergeFiles()) {
        if (!opt.quiet) printf("  错误: %s\n", task.error_.c_str());
        CleanupFailedDownload(local_path);  // 合并失败同样清理
        return 1;  // 可重试
    }
    if (opt.on_progress) opt.on_progress(task.file_size(), task.file_size(), speed.load());
    return 0;  // 成功
}

}  // namespace pcl_dl
