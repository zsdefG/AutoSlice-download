// downloader.h - PCL 风格多线程分片下载引擎 (C++17)
// 复刻 Plain Craft Launcher 2 (ModNet.vb) 的核心提速机制：
//   1. 单文件分片并行下载 (HTTP Range)
//   2. 动态加线程: 全局速度低于阈值时, 从最大剩余碎片中间 40% 处切分
//   3. 多源镜像自动切换 + 断点续传
//   4. 全局限速 (令牌桶)
// 依赖: WinHTTP (Windows 原生, 零第三方依赖)
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pcl_dl {

// ===== 下载源 (对应 PCL NetSource) =====
struct Source {
    std::string url;
    int id = 0;
    std::atomic<bool> failed{false};
};

// ===== 分片 (对应 PCL NetThread) =====
struct Piece {
    std::int64_t start = 0;       // 起始偏移 (固定)
    std::string part_path;        // 分片临时文件
    std::atomic<std::int64_t> done{0};  // 已下载字节
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};

    Piece(std::int64_t s, std::string path) : start(s), part_path(std::move(path)) {}
};

// ===== 文件下载任务 (对应 PCL NetFile) =====
class DownloadTask {
public:
    DownloadTask(std::vector<std::string> urls, std::string local_path);

    // 探测文件大小, 决定是否分片 (返回 false 表示失败)
    bool ProbeSize();
    // 是否无需分片 (< 1MB 或服务器不支持 Range)
    bool IsNoSplit() const { return no_split_; }
    // 大小未知 (chunked/流式响应, 无 Content-Length) — 对应 PCL IsUnknownSize
    bool IsUnknownSize() const { return file_size_ < 0; }
    std::int64_t file_size() const { return file_size_; }

    // 当前片在链表中的结束位置 (下一个片的 start - 1)。调用方必须持有 chain_mtx。
    std::int64_t PieceEndLocked(const Piece& p) const;
    std::int64_t PieceEnd(const Piece& p) const;

    // 挑选可用源 (轮询, 跳过已失败的)
    Source* GetSource(std::int64_t from_id);
    // 是否存在可用源
    bool HasAvailableSource() const;

    // 全部分片完成时合并到目标文件
    bool MergeFiles();

    // 状态查询
    std::int64_t TotalDone() const;
    bool AllFinished() const;
    bool HasFailed() const { return task_failed_; }
    std::string error() const { return error_; }

    std::vector<std::unique_ptr<Piece>> pieces;  // 有序链表 (按 start)
    mutable std::mutex chain_mtx;                // 保护 pieces 链表
    mutable std::mutex src_mtx;                  // 保护源轮询
    std::string local_path;
    std::string error_;
    std::atomic<bool> task_failed_{false};
    std::int64_t file_size_ = -2;                // -2 未探测, -1 未知大小
    bool no_split_ = true;
    std::vector<std::unique_ptr<Source>> sources;
    std::atomic<std::int64_t> first_thread_src{0};

    // 下载成功的源序号 → 用于"首个线程"源分配
    std::int64_t probe_thread_id = 0;

private:
    bool ProbeWithRange(std::int64_t* size_out, bool* supports_range);
};

// ===== 下载引擎配置 =====
struct DownloadOptions {
    int max_threads = 0;                   // 最大并发线程数; 0 = 自动 (硬上限 32)
    std::int64_t per_thread_min = 32 * 1024;   // 每线程速度下限: 低于此值视为带宽饱和, 不再加线程 (B/s)
    std::int64_t piece_limit = 256 * 1024;     // 最小碎片: 小于此值不再切分
    std::int64_t split_ratio_num = 40;         // 从碎片剩余量的 40% 处切分
    std::int64_t speed_cap = -1;               // 全局限速 (B/s); -1 不限速; 0 = 自动 (按实测带宽上限的 80%)
    int poll_ms = 20;                          // 调度轮询间隔
    int max_retries = 3;                       // 下载失败后的总尝试次数 (含首次; 1 = 不重试)
    int retry_delay_ms = 1000;                 // 每次重试前等待毫秒
    std::atomic<bool>* cancel_flag = nullptr;  // 非空时, 调度/下载循环定期检查, true 则立即取消
    bool quiet = false;                        // 不打印日志
    std::function<void(std::int64_t done, std::int64_t total, std::int64_t speed_bps)> on_progress;
};

// ===== 下载引擎 (对应 PCL NetManager) =====
class DownloadEngine {
public:
    // 下载单个文件 (多源), 阻塞直到完成。失败时按 opt.max_retries 自动重试。
    // 返回 true 表示成功。
    static bool Download(std::vector<std::string> urls, const std::string& local_path,
                         const DownloadOptions& opt = DownloadOptions{});

    // 单次下载尝试 (无重试), 供 Download 内部使用。
    // 返回 0=成功, 1=可重试失败(下载中断), 2=不可重试失败(探测失败)
    static int DownloadOnce(std::vector<std::string> urls, const std::string& local_path,
                            const DownloadOptions& opt);

    // 删除 <文件名>.part* 分片残留 (不动目标文件本身)。
    // 供 Ctrl+C 处理器等外部场景调用, 确保中断时也能清理。
    static void CleanupPartFiles(const std::string& local_path);
    // 清理分片 + 半成品目标文件
    static void CleanupFailedDownload(const std::string& local_path);

    // 供 DownloadTask 使用: 尝试启动一个分片线程
    bool TryBeginThread(DownloadTask& task);

    std::atomic<int> thread_count{0};
    DownloadOptions opt;
    std::atomic<std::int64_t> tokens{0};  // 限速令牌桶余量

    // 自适应统计 (对应 PCL FUTURE: 计算下载源平均链接时间和线程下载速度, 按最高时间节省开启多线程)
    std::atomic<std::int64_t> connect_total_ms{0};  // 所有线程连接耗时之和
    std::atomic<int> connect_count{0};              // 已统计连接数的线程
    std::atomic<std::int64_t> measured_bandwidth{0};  // 实测带宽上限 (B/s), 用于自动限速
    std::int64_t AvgConnectMs() const;              // 平均连接耗时 (无样本时返回 0)

private:
    struct Ctx {
        DownloadTask* task;
        Piece* piece;
        DownloadEngine* engine;
    };
    static void Worker(Ctx ctx);
};

// ===== 工具 =====
std::string WsToUtf8(const std::wstring& w);
std::wstring Utf8ToWs(const std::string& s);
std::string GetLastErrorStr();

// ===== HTTP 层 (WinHTTP 封装) =====
// 发起 Range 下载。start<0 表示不带 Range。
// 成功返回 true; 返回体通过 on_data 回调 (返回 false 表示提前停止读取, 视为成功)。
bool HttpDownloadRange(const std::string& url,
                       std::int64_t start, std::int64_t end,
                       const std::function<bool(const char* data, size_t len)>& on_data,
                       std::string* err_out,
                       std::int64_t speed_cap,   // 全局令牌桶
                       std::atomic<std::int64_t>& token_left);

}  // namespace pcl_dl
