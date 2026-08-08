# -*- coding: utf-8 -*-
# chunked_server.py - 用 Transfer-Encoding: chunked 响应, 无 Content-Length
# 用于验证下载引擎对未知大小文件 (chunked) 的处理
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class ChunkedHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stdout.write(f"[server] {fmt % args}\n")

    def do_GET(self):
        # 生成 2MB 随机数据
        import random
        random.seed(777)
        data = bytes(random.getrandbits(8) for _ in range(2 * 1024 * 1024))

        # chunked 编码响应 (无 Content-Length)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        CHUNK = 64 * 1024
        for i in range(0, len(data), CHUNK):
            piece = data[i:i + CHUNK]
            self.wfile.write(f"{len(piece):X}\r\n".encode())
            self.wfile.write(piece)
            self.wfile.write(b"\r\n")
            self.wfile.flush()
            time.sleep(0.005)  # 轻微限速
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8133
    httpd = ThreadingHTTPServer(("127.0.0.1", port), ChunkedHandler)
    print(f"Chunked test server on http://127.0.0.1:{port}/")
    httpd.serve_forever()
