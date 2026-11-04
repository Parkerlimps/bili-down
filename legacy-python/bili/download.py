"""DASH 分片下载与合流。

B 站的音视频是两条独立的 fMP4 流，必须先分别下载再用 ffmpeg 合成 mp4。
下载采用 Range 分块并发：先探测总长度，预分配文件，多线程各自 seek 写入。
"""

from __future__ import annotations

import os
import queue
import re
import subprocess
import threading
import time
from collections import deque
from pathlib import Path
from typing import Any, Callable

import requests

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)

CHUNK = 4 * 1024 * 1024  # 单块 4MB
THREADS = 8


class DownloadError(Exception):
    pass


class Cancelled(Exception):
    pass


# ---------------- ffmpeg ----------------


def find_ffmpeg(base_dir: str | Path) -> str | None:
    """优先用项目 bin 里自带的 ffmpeg，其次找 PATH。"""
    base = Path(base_dir)
    for p in sorted(base.glob("bin/**/ffmpeg.exe")) + sorted(base.glob("bin/**/ffmpeg")):
        if p.is_file():
            return str(p)
    import shutil

    return shutil.which("ffmpeg")


# ---------------- 分块下载 ----------------


class _Progress:
    """滑动窗口算实时速度，避免瞬时抖动。"""

    def __init__(self, total: int, cb: Callable[[int, int, float], None] | None, every: float = 0.35):
        self.total = total
        self.done = 0
        self.cb = cb
        self.every = every
        self.samples: deque[tuple[float, int]] = deque()
        self.last_emit = 0.0
        self.lock = threading.Lock()

    def add(self, n: int) -> None:
        with self.lock:
            self.done += n
            now = time.time()
            self.samples.append((now, self.done))
            while len(self.samples) > 2 and now - self.samples[0][0] > 3:
                self.samples.popleft()
            if self.cb and now - self.last_emit >= self.every:
                self.last_emit = now
                self._emit(now)

    def _emit(self, now: float) -> None:
        if len(self.samples) >= 2:
            t0, b0 = self.samples[0]
            speed = (self.done - b0) / max(now - t0, 1e-6)
        else:
            speed = 0.0
        if self.cb:
            self.cb(self.done, self.total, speed)

    def flush(self) -> None:
        with self.lock:
            self._emit(time.time())


def probe_size(url: str, headers: dict[str, str], session: requests.Session) -> int:
    """用 Range 0-0 探测文件总长，读 Content-Range。"""
    r = session.get(url, headers={**headers, "Range": "bytes=0-0"}, stream=True, timeout=15)
    try:
        if r.status_code not in (200, 206):
            raise DownloadError(f"探测文件大小失败：HTTP {r.status_code}")
        cr = r.headers.get("Content-Range")
        if cr and "/" in cr:
            total = cr.rsplit("/", 1)[1].strip()
            if total.isdigit():
                return int(total)
        cl = r.headers.get("Content-Length")
        if cl and cl.isdigit() and r.status_code == 200:
            return int(cl)
        raise DownloadError("无法确定文件大小")
    finally:
        r.close()


def download_url(
    urls: list[str],
    out_path: Path,
    headers: dict[str, str],
    progress: _Progress,
    cancel: threading.Event | None = None,
    threads: int = THREADS,
    on_log: Callable[[str], None] | None = None,
) -> None:
    """把一个远程文件分块并发下到 out_path。urls 为备用地址列表。"""
    sess = requests.Session()
    sess.trust_env = False
    sess.headers.update({"User-Agent": UA, "Referer": "https://www.bilibili.com/"})

    url = urls[0]
    total = probe_size(url, headers, sess)
    progress.total = total
    if on_log:
        on_log(f"目标大小 {total / 1048576:.1f} MB")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.truncate(total)

    blocks: queue.Queue[tuple[int, int]] = queue.Queue()
    start = 0
    while start < total:
        end = min(start + CHUNK, total) - 1
        blocks.put((start, end))
        start = end + 1

    errors: list[Exception] = []
    lock = threading.Lock()

    def worker() -> None:
        fh = open(out_path, "r+b")
        try:
            while True:
                if cancel and cancel.is_set():
                    return
                try:
                    s, e = blocks.get_nowait()
                except queue.Empty:
                    return
                got = 0
                for attempt in range(4):
                    try:
                        cur = s + got
                        if cur > e:
                            break
                        rs = sess.get(
                            url,
                            headers={**headers, "Range": f"bytes={cur}-{e}"},
                            stream=True,
                            timeout=25,
                        )
                        if rs.status_code not in (200, 206):
                            raise DownloadError(f"HTTP {rs.status_code}")
                        fh.seek(cur)
                        for data in rs.iter_content(262144):
                            if cancel and cancel.is_set():
                                rs.close()
                                return
                            if data:
                                fh.write(data)
                                got += len(data)
                                progress.add(len(data))
                        rs.close()
                        if got >= e - s + 1:
                            break
                        raise DownloadError("分块未下完")
                    except Exception as ex:
                        if attempt == 3:
                            with lock:
                                errors.append(ex)
                            return
                        time.sleep(0.6 * (attempt + 1))
        finally:
            fh.close()

    pool = [threading.Thread(target=worker, daemon=True) for _ in range(min(threads, max(1, total // 524288 + 1)))]
    for t in pool:
        t.start()
    for t in pool:
        t.join()

    if cancel and cancel.is_set():
        raise Cancelled()
    if errors:
        raise DownloadError(f"下载失败：{errors[0]}")
    if out_path.stat().st_size != total:
        raise DownloadError("文件大小不符，可能被中断")


# ---------------- 字幕 ----------------


def json_to_srt(sub: dict) -> str:
    def ts(sec: float) -> str:
        h = int(sec // 3600)
        m = int(sec % 3600 // 60)
        s = int(sec % 60)
        ms = int(round((sec - int(sec)) * 1000))
        return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"

    lines = []
    for i, item in enumerate(sub.get("body") or [], 1):
        content = (item.get("content") or "").strip()
        if not content:
            continue
        lines.append(f"{i}\n{ts(item.get('from', 0))} --> {ts(item.get('to', 0))}\n{content}\n")
    return "\n".join(lines)


# ---------------- 合并 ----------------


def merge(
    video: Path,
    audio: Path | None,
    out: Path,
    ffmpeg: str,
    subtitle: Path | None = None,
    audio_ok_for_mp4: bool = True,
) -> None:
    cmd = [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", str(video)]
    if audio:
        cmd += ["-i", str(audio)]
    if subtitle:
        cmd += ["-i", str(subtitle)]
    cmd += ["-map", "0:v:0"]
    if audio:
        cmd += ["-map", "1:a:0"]
    if subtitle:
        cmd += ["-map", "2:s:0"]
    if audio and audio_ok_for_mp4:
        cmd += ["-c", "copy"]
    elif audio:
        cmd += ["-c:v", "copy", "-c:a", "aac", "-b:a", "320k"]
    else:
        cmd += ["-c", "copy"]
    if subtitle:
        cmd += ["-c:s", "mov_text", "-metadata:s:s:0", "language=chi"]
    cmd += ["-movflags", "+faststart", str(out)]

    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise DownloadError(f"ffmpeg 合并失败：{(p.stderr or '').strip()[:400]}")


def duration_of(path: Path, ffmpeg: str) -> float:
    ffprobe = str(Path(ffmpeg).with_name("ffprobe.exe" if os.name == "nt" else "ffprobe"))
    if not Path(ffprobe).exists():
        return 0.0
    try:
        p = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format=duration",
             "-of", "default=nw=1:nk=1", str(path)],
            capture_output=True, text=True, timeout=30,
        )
        return float((p.stdout or "0").strip() or 0)
    except Exception:
        return 0.0


def safe_name(s: str, limit: int = 100) -> str:
    s = re.sub(r'[\\/:*?"<>|\r\n\t]', " ", s or "")
    s = re.sub(r"\s+", " ", s).strip().strip(".")
    return (s[:limit] or "video")
