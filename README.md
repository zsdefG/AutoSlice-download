# AutoSlice-download

多段式分片下载引擎（C++17 / WinHTTP），带原生 Windows 图形界面。

参考 PCL（Plain Craft Launcher）启动器下载模块设计：动态分片并行、按实测带宽自适应线程数、多源镜像容错、断点续传、限速、失败自动重试与残留清理。**全程零第三方依赖，仅使用 Windows 原生 API。**

## 特性

- **双界面** — 命令行 `downloader.exe` 与原生 Win32 GUI `downloadergui.exe`，共用同一引擎
- **自适应多线程** — 引擎按实测带宽自动决定线程数，无需手动配置
- **动态分片** — 支持 Range 的服务器自动分片并行下载，碎片下限 256 KB，完成后精确合并
- **多源容错** — 多 URL 并行探测，失效源自动跳过，下载中断流自动切换
- **未知大小文件** — 服务器返回 chunked（无 Content-Length）时自动降级单线程流式下载
- **三态限速** — 不限速 / 固定限速 / 自动限速（先满速探测带宽，按 80% 限速）
- **智能重试** — 4xx 立即失败不浪费时间，5xx 与网络错误自动重试
- **零残留保证** — 失败/中断/Ctrl+C/GUI 取消/关窗全部自动清理分片
- **中文路径** — 引擎全宽字符（UTF-16）路径，中文目录文件名无乱码
- **可视化进度** — CLI 单行动态进度条，GUI 实时百分比 + 速度

## 快速开始

### 方式一：使用 GUI（推荐）

```powershell
downloadergui.exe
```

| 控件 | 说明 |
|------|------|
| 下载地址 | 每行一个 URL，多源自动容错 |
| 输出路径 | 留空自动取 URL 文件名；"浏览..."保存对话框默认文件名与 URL 一致 |
| 线程上限 | `0` = 自动（按带宽自适应，硬上限 32） |
| 限速 KB/s | `-1` 不限速 / `0` 自动（实测带宽 80%）/ 正数固定限速 |
| 重试 / 间隔 | 失败后总尝试次数与重试间隔（毫秒） |
| 开始 / 取消 | 取消立即中断并清理分片 |
| 日志区 | 源列表、HTTP 状态码、网络错误码 |

### 方式二：命令行

```powershell
downloader.exe <url> [url2 ...] [输出路径] [选项]
```

```powershell
# 最简用法（输出名自动取 URL 文件名）
downloader.exe https://example.com/file.bin

# 多源容错
downloader.exe http://mirror-a.com/f.bin http://mirror-b.com/f.bin out.bin

# 固定限速 + 限制线程
downloader.exe https://example.com/file.bin out.bin --speed-cap 1024 --threads 8

# 自动限速（实测带宽 80%）
downloader.exe https://example.com/file.bin out.bin --speed-cap 0

# 失败重试配置
downloader.exe https://example.com/file.bin out.bin --retry 5 --retry-delay 2000

# 中文路径
downloader.exe https://example.com/file.bin "D:\桌面\file.bin"
```

#### 命令行选项

| 参数 | 说明 |
|------|------|
| `-h, --help` | 显示帮助 |
| `--threads N` | 最大线程数上限（默认 0 = 自动，硬上限 32） |
| `--speed-cap KB/s` | 限速：`-1` 不限速（默认）；`0` 自动；`>0` 固定 |
| `--per-thread KB/s` | 每线程速度下限，低于此判定带宽饱和（默认 32） |
| `--retry N` | 失败后总尝试次数，含首次（默认 3，1 = 不重试） |
| `--retry-delay ms` | 每次重试前等待毫秒（默认 1000） |
| `--quiet` | 静默模式 |
| `--self-test [目录]` | 生成 12MB 测试文件 |

## 效果预览

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

## 构建

依赖：MSYS2（ucrt64 工具链）+ CMake + Ninja。

```bash
cmake -S . -B build -G Ninja
cmake --build build
# 产物:
#   build/downloader.exe      # 命令行版
#   build/downloadergui.exe   # Win32 GUI 版
```

> 项目路径含中文时必须用 Ninja（mingw32-make 在中文路径下会乱码报错）。

## 工作原理

### 自适应线程算法

调度线程每 20ms 评估一次，四个条件**同时满足**才加线程：

| 条件 | 逻辑 |
|------|------|
| 每线程速度下限 | 总速度/线程数 < 32KB/s → 判定带宽饱和，记录实测带宽 |
| 收益评估 | 加线程节省的时间 > 平均连接耗时（否则不值得加） |
| 边际收益 | 加线程后速度提升 < 8% 且连续 5 次 → 判定饱和停止 |
| 连接耗时统计 | 每个 worker 记录"线程创建 → 首字节"耗时 |

### 重试策略

| 错误类型 | 是否重试 | 原因 |
|---------|---------|------|
| HTTP 4xx（404 等） | 否 | 确定错误，重试无意义 |
| HTTP 5xx | 是 | 可能是临时故障 |
| 网络错误（连接失败、超时） | 是 | 可能是网络抖动 |
| URL 解析失败 | 否 | 参数错误 |

### 残留清理机制

失败 / 重试耗尽 / 合并失败 / Ctrl+C / GUI 取消 / 关窗 / 正常退出（atexit）→ 自动清理 `.part*` 分片与半成品；下次运行自动清历史残留。

> 唯一例外：任务管理器强杀进程（TerminateProcess）后无法执行任何代码，残留只能靠下次运行清理——Windows 进程模型的固有限制。

## 项目结构

```
AutoSlice-download/
├── CMakeLists.txt
├── src/
│   ├── main.cpp         # CLI 入口（wmain）、进度条、Ctrl+C 处理
│   ├── gui.cpp          # Win32 GUI：控件布局、进度回调、浏览对话框
│   ├── downloader.cpp   # 下载引擎：探测/分片/调度/合并/重试/清理
│   └── downloader.h
├── test/
│   ├── range_server.py     # Range 测试服务器（限速 / 前 N 请求返回 500）
│   ├── chunked_server.py   # chunked 响应测试服务器
│   └── lifecycle_test.py   # 下载生命周期清理测试
└── build/               # 构建产物
```

## 测试

```bash
# 起测试服务器（test 目录）
python range_server.py 8123 . 0          # 不限速
python range_server.py 8123 . 100000     # 每连接限速 100KB/s
python range_server.py 8123 . 0 2        # 前 2 个请求返回 500（测重试）

# 下载并校验
downloader.exe http://127.0.0.1:8123/big_src.bin out.bin --retry 3 --retry-delay 500
```

已覆盖：哈希匹配、慢速服务器自动加线程提速约 10 倍、坏源排前自动切换、chunked 流式下载、404 快速失败、500 重试成功、下载中断零残留、Ctrl+C 中断清理、中文路径下载、GUI 取消/关窗清理。

## 许可证

MIT License
