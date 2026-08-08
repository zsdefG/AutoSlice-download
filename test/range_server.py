# range_server.py - 支持 HTTP Range 的测试服务器
# 用法: python range_server.py <端口> <目录>
# 用于验证分片下载引擎: 服务器必须正确响应 Range 请求 (206)
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class RangeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    directory = "."
    speed_limit = 0  # B/s, 0 = 不限速
    fail_first_n = 0  # 前 N 个请求返回 500 (模拟临时故障), 之后正常

    def log_message(self, fmt, *args):
        # 精简日志, 每 10 次 Range 请求打印一次
        sys.stdout.write(f"[server] {fmt % args}\n")

    def do_GET(self):
        # 前 N 个请求模拟临时故障 (返回 500)
        if RangeHandler.fail_first_n > 0:
            RangeHandler.fail_first_n -= 1
            self.send_response(500)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        path = self.path.split("?")[0].lstrip("/")
        full = os.path.normpath(os.path.join(self.directory, path))
        if not full.startswith(os.path.normpath(self.directory)) or not os.path.isfile(full):
            self.send_error(404)
            return
        size = os.path.getsize(full)
        rng = self.headers.get("Range")

        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng)
            if not m:
                self.send_error(400)
                return
            start_s, end_s = m.group(1), m.group(2)
            start = int(start_s) if start_s else 0
            end = int(end_s) if end_s else size - 1
            if start >= size:
                self.send_error(416)
                return
            end = min(end, size - 1)
            length = end - start + 1
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(full, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(65536, remaining))
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                        self.wfile.flush()
                    except BrokenPipeError:
                        return
                    remaining -= len(chunk)
                    if RangeHandler.speed_limit > 0:
                        # 按每 64KB 块限速
                        delay = len(chunk) / RangeHandler.speed_limit
                        time.sleep(delay)
        else:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(full, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except BrokenPipeError:
                        return


class Server(ThreadingHTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    RangeHandler.directory = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else ".")
    if len(sys.argv) > 3:
        RangeHandler.speed_limit = int(sys.argv[3])  # B/s 限速
    if len(sys.argv) > 4:
        RangeHandler.fail_first_n = int(sys.argv[4])  # 前 N 个请求失败
    httpd = Server(("127.0.0.1", port), RangeHandler)
    print(f"Range test server on http://127.0.0.1:{port}/ serving {RangeHandler.directory}"
          f" speed_limit={RangeHandler.speed_limit} fail_first_n={RangeHandler.fail_first_n}")
    httpd.serve_forever()
