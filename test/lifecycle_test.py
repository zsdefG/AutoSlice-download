# lifecycle_test.py - 完整下载生命周期清理测试
# 流程: 启动慢速下载 -> 杀服务器(网络故障) -> 等待失败清理 -> 检查残留
import subprocess, time, glob, os

def main():
    # 清理旧文件
    for f in glob.glob('outLife*'):
        try: os.remove(f)
        except OSError: pass

    # 1. 启动下载 (慢速 300MB, 100+ 秒才能完成)
    proc = subprocess.Popen(
        [r'D:\文档\workbuddy\pydemo\cpp_downloader\build\downloader.exe',
         'http://127.0.0.1:8123/big_src.bin', 'outLife.bin', '--retry', '1'],
        cwd=r'D:\文档\workbuddy\pydemo\cpp_downloader\test',
        stdout=open('dlLife.txt', 'wb'), stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
    )
    time.sleep(8)
    parts = glob.glob('outLife.bin.part*')
    print(f'[1] 下载 8 秒后: 分片数 = {len(parts)}, 进程运行中 = {proc.poll() is None}')

    # 2. 杀服务器 (模拟网络故障) - 用 taskkill 杀所有监听 8123 的 python
    print('[2] 杀掉服务器...')
    os.system('for /f "tokens=5" %a in (\'netstat -ano ^| findstr ":8123" ^| findstr "LISTENING"\') do taskkill /F /PID %a >nul 2>&1')

    # 3. 等待下载器失败并自动清理
    time.sleep(12)
    rc = proc.poll()
    print(f'[3] 下载器退出码 = {rc}')

    # 4. 检查残留
    parts = glob.glob('outLife.bin.part*')
    out = glob.glob('outLife.bin')
    print(f'[4] 残留分片数 = {len(parts)}, 半成品输出 = {len(out)}')
    if not parts and not out and rc is not None:
        print('结论: 通过 - 下载失败后当场清理, 零残留')
    elif not parts and not out:
        print('结论: 分片已清理但进程未退出 (可能还在重试)')
    else:
        print(f'结论: 失败 - 残留 {len(parts)} 分片, {len(out)} 输出')

if __name__ == '__main__':
    main()
