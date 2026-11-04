"""BiliDown 入口。

用法：
    python app.py                # 启动并自动打开浏览器
    python app.py --port 9000    # 指定端口
    python app.py --no-browser   # 不自动打开浏览器
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))


def main() -> None:
    ap = argparse.ArgumentParser(description="BiliDown - B 站视频下载工具")
    ap.add_argument("--port", type=int, default=8760, help="本地服务端口，默认 8760")
    ap.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    args = ap.parse_args()

    from server import run

    run(port=args.port, open_browser=not args.no_browser)


if __name__ == "__main__":
    main()
