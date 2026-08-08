# AutoSlice-download

C++17 / WinHTTP 实现的多段式分片下载引擎，参考 PCL（Plain Craft Launcher）启动器源码的下载模块设计。提供**命令行 CLI** 和 **原生 Win32 图形界面（GUI）**，支持多源容错、断点续传、自适应线程数、限速、失败自动重试与残留清理。

## 功能特性

- **双界面**：CLI（`downloader.exe`）+ 原生 Win32 GUI（`downloadergui.exe`），复用同一引擎
- **多源容错**：同时传入多个 URL，探测时自动跳过失效源，下载中源断流自动切换下一个源
- **自适应线程数**：引擎按实测带宽自动决定线程数，无需手动配置（可设上限覆盖）
- **分片下载**：支持 Range 的服务器自动分片并行下载（碎片下限 256 KB），下载完成后合并
- **未知大小文件**：服务器返回 chunked（无 Content-Length）时自动切换单线程流式下载
- **三态限速**：不限速 / 固定限速 / 自动限速（先满速探测带宽，按实测带宽 80% 限速）
- **失败自动重试**：临时性错误（网络中断、HTTP 5xx）自动重试，确定错误（404 等）立即失败
- **失败自动清理**：下载失败/中断自动删除 `.part*` 分片和半成品，Ctrl+C 中断同样清理，下次运行自动清理历史残留
- **中文路径支持**：引擎内部全宽字符（UTF-16）路径，中文目录/文件名无乱码
- **可视化进度条**：CLI 终端单行动态刷新 `[██████░░░░] 84.20% 252.61 MB / 300.00 MB | 2967.0 KB/s`
- **默认输出名**：省略输出路径时自动取 URL 文件名（剔除 query/fragment）

## 构建

依赖：MSYS2（ucrt64 工具链）+ CMake + Ninja，链接 winhttp / comctl32。

```bash
# 在项目根目录（MSYS2 环境，或确保 PATH 包含 ucrt64/bin）
cmake -S . -B build -G Ninja
cmake --build build
# 产物:
#   build/downloader.exe      # 命令行版
#   build/downloadergui.exe   # Win32 GUI 版
```

注意：项目路径含中文时，必须用 Ninja 构建（mingw32-make 在中文路径下会乱码报错）。

## 使用

### GUI 版（推荐）

```
downloadergui.exe
```

| 控件 | 说明 |
|------|------|
| 下载地址 | 每行一个 URL，支持多源容错 |
| 输出路径 | 可留空（自动取 URL 文件名）；点"浏览..."用保存对话框选择，默认文件名自动与 URL 一致 |
| 线程上限 | 0 = 自动（引擎按带宽自适应，硬上限 32） |
| 限速 KB/s | -1 不限速；0 自动（实测带宽 80%）；>0 固定限速 |
| 重试 / 间隔ms | 失败后总尝试次数与重试间隔 |
| 开始下载 / 取消 | 取消会中断下载并自动清理分片 |
| 进度条 + 状态 | 实时百分比 / 大小 / 速度；未知大小文件显示"已下载 MB (未知大小)" |
| 日志区 | 显示源列表、错误信息（含 HTTP 状态码 / 网络错误码） |

### CLI 版

```
downloader.exe <url> [url2 ...] [输出路径] [选项]
```

| 参数 | 说明 |
|------|------|
| `-h, --help` | 显示帮助 |
| `--threads N` | 最大线程数上限（默认 0 = 自动，硬上限 32） |
| `--speed-cap KB/s` | 限速：`-1` 不限速（默认）；`0` 自动按实测带宽 80% 限速；`>0` 固定限速 |
| `--per-thread KB/s` | 每线程速度下限，低于此判定带宽饱和（默认 32） |
| `--retry N` | 失败后总尝试次数，含首次（默认 3，1 = 不重试） |
| `--retry-delay ms` | 每次重试前等待毫秒（默认 1000） |
| `--quiet` | 静默模式，不打印进度和自适应日志 |

CLI 示例：

```bash
# 基本下载（输出名自动取 URL 文件名）
downloader.exe https://example.com/file.bin

# 指定输出路径 + 多源容错
downloader.exe http://mirror-a.com/f.bin http://mirror-b.com/f.bin out.bin

# 固定限速 + 限制线程
downloader.exe https://example.com/file.bin out.bin --speed-cap 1024 --threads 8

# 自动限速（按实测带宽 80%）
downloader.exe https://example.com/file.bin out.bin --speed-cap 0

# 失败重试配置
downloader.exe https://example.com/file.bin out.bin --retry 5 --retry-delay 2000

# 中文路径（Windows 10 1809+ 终端）
downloader.exe https://example.com/file.bin "D:\桌面\file.bin"

# 自测：生成 12MB 测试文件
downloader.exe --self-test .
```

CLI 输出示例：

```
  源: 1 个
  输出: demo.bin
  线程上限: 自动 (最多 32)
  每线程速度下限: 32 KB/s  碎片下限: 256 KB
  限速: 不限
  重试: 3 次 (间隔 1000ms)
  探测文件大小...
  [██████████████████░░░░░░]  84.20%  252.61 MB / 300.00 MB | 2967.0 KB/s
完成: demo.bin (300.00 MB, 101.2 s, 平均 2965 KB/s)
```

## 自适应线程算法

调度线程每 20ms 评估是否加线程，四个条件同时满足才加：

| 条件 | 逻辑 |
|------|------|
| 每线程速度下限 | `总速度/线程数 < 32KB/s` → 带宽饱和，记录实测带宽 |
| 收益评估 | 节省时间 = `剩余量/当前速度 − 剩余量/(当前速度+每线程速度)`，若节省 < 平均连接耗时 → 不值得加 |
| 边际收益 | 加线程后速度提升 < 8% 且连续 5 次 → 判定饱和停止 |
| 连接耗时统计 | 每个 worker 记录"线程创建 → 首字节"耗时 |

## 重试策略

| 错误类型 | 是否重试 | 原因 |
|---------|---------|------|
| HTTP 4xx（404 等） | 否 | 确定错误，重试无意义 |
| HTTP 5xx | 是 | 可能是临时故障 |
| 网络错误（连接失败、超时） | 是 | 可能是网络抖动 |
| URL 解析失败 | 否 | 参数错误 |

## 残留清理机制

- **失败自动清理**：下载中断、重试耗尽、合并失败 → 删除 `.part*` 分片 + 半成品
- **探测失败保护**：只清分片，不删除目标文件（目标可能是用户已有数据）
- **Ctrl+C 清理**：捕获 Ctrl+C / 关窗口信号，取消下载并清理后再退出
- **GUI 取消/关窗清理**：取消下载或关闭窗口时自动清理分片
- **atexit 兜底**：任何正常退出路径（return/exit）都会清理
- **下次运行清理**：启动时自动删除同输出名的历史 `.part*` 残留

> 限制：任务管理器强杀进程（TerminateProcess）后无法执行任何代码，残留只能靠下次运行清理，这是 Windows 进程模型的固有限制。

## 项目结构

```
AutoSlice-download/
├── CMakeLists.txt
├── src/
│   ├── main.cpp         # CLI 入口（wmain）、进度条、Ctrl+C 处理
│   ├── gui.cpp          # Win32 GUI：控件布局、进度回调、浏览对话框
│   ├── downloader.cpp   # 下载引擎：探测/分片/调度/合并/重试/清理（宽字符路径）
│   └── downloader.h
├── test/
│   ├── range_server.py     # Range 测试服务器（支持限速、前 N 请求返回 500）
│   ├── chunked_server.py   # chunked 响应测试服务器（无 Content-Length）
│   └── lifecycle_test.py   # 下载生命周期清理测试
└── build/               # 构建产物（downloader.exe / downloadergui.exe）
```

## 测试

```bash
# 1. 起测试服务器（test 目录）
python range_server.py 8123 . 0          # 不限速
python range_server.py 8123 . 100000     # 每连接限速 100KB/s
python range_server.py 8123 . 0 2        # 前 2 个请求返回 500（测重试）

# 2. 下载并校验
downloader.exe http://127.0.0.1:8123/big_src.bin out.bin --retry 3 --retry-delay 500

# 3. 中文路径验证
downloader.exe http://127.0.0.1:8123/big_src.bin "D:\桌面\test.bin"
```

已覆盖验证：12MB/300MB 哈希匹配、慢速服务器自动加线程提速约 10 倍、坏源排前自动切换、chunked 流式下载、404 快速失败、2 次 500 后重试成功、下载中断零残留、Ctrl+C 中断清理、中文路径下载哈希匹配、GUI 取消/关窗清理。
