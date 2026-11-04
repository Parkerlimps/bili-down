"""本地 HTTP 服务：解析接口 + 下载任务队列 + 静态页面。

刻意不引入 web 框架，标准库 http.server 足够，双击就能跑起来。
"""

from __future__ import annotations

import base64
import io
import json
import os
import queue
import threading
import time
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from bili.client import BiliClient, BiliError, QUALITY_NAMES
from bili.download import (
    Cancelled,
    DownloadError,
    _Progress,
    download_url,
    duration_of,
    find_ffmpeg,
    json_to_srt,
    merge,
    safe_name,
)

ROOT = Path(__file__).resolve().parent
WEB = ROOT / "web"
DATA = ROOT / "data"
TMP = ROOT / "data" / ".tmp"
COOKIE_FILE = ROOT / "cookie.json"
SETTINGS_FILE = ROOT / "settings.json"

DATA.mkdir(exist_ok=True)
TMP.mkdir(parents=True, exist_ok=True)

CLIENT = BiliClient(str(COOKIE_FILE))
FFMPEG = find_ffmpeg(ROOT)


def load_settings() -> dict:
    if SETTINGS_FILE.exists():
        try:
            return json.loads(SETTINGS_FILE.read_text("utf-8"))
        except Exception:
            pass
    return {"out_dir": str(DATA), "threads": 8}


SETTINGS = load_settings()


def save_settings() -> None:
    SETTINGS_FILE.write_text(json.dumps(SETTINGS, ensure_ascii=False, indent=2), "utf-8")


# ---------------- 任务 ----------------


class Task:
    __slots__ = (
        "id", "bvid", "cid", "title", "part", "page", "qn", "codecid",
        "sub_lan", "status", "phase", "downloaded", "total", "speed",
        "pct", "eta", "file", "error", "log", "cancel",
        "created_at", "finished_at", "_speed_ts", "_speed_bytes",
    )

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, None)
        self.log = []
        self.cancel = threading.Event()
        self.downloaded = 0
        self.total = 0
        self.speed = 0.0
        self.pct = 0.0
        self.eta = 0.0
        self.status = "queued"
        self.phase = "等待中"
        self.created_at = time.time()
        self._speed_ts = time.time()
        self._speed_bytes = 0
        for k, v in kw.items():
            setattr(self, k, v)

    def add_log(self, msg: str) -> None:
        self.log.append(f"[{time.strftime('%H:%M:%S')}] {msg}")
        self.log[:] = self.log[-80:]

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "bvid": self.bvid,
            "title": self.title,
            "part": self.part,
            "page": self.page,
            "qn": self.qn,
            "qn_name": QUALITY_NAMES.get(self.qn, str(self.qn)),
            "status": self.status,
            "phase": self.phase,
            "downloaded": self.downloaded,
            "total": self.total,
            "speed": round(self.speed, 1),
            "pct": round(self.pct, 1),
            "eta": round(self.eta, 1),
            "file": self.file,
            "error": self.error,
            "log": self.log[-12:],
            "created_at": self.created_at,
            "finished_at": self.finished_at,
        }


class Manager:
    def __init__(self, limit: int = 2):
        self.tasks: dict[str, Task] = {}
        self.order: list[str] = []
        self.sem = threading.Semaphore(limit)
        self.lock = threading.Lock()
        self.seq = 0

    def new_id(self) -> str:
        self.seq += 1
        return f"{int(time.time())}{self.seq:02d}"

    def submit(self, task: Task) -> None:
        with self.lock:
            self.tasks[task.id] = task
            self.order.insert(0, task.id)
        threading.Thread(target=self._run, args=(task,), daemon=True).start()

    def get(self, tid: str) -> Task | None:
        return self.tasks.get(tid)

    def all(self) -> list[dict]:
        return [self.tasks[t].to_dict() for t in self.order if t in self.tasks]

    def cancel(self, tid: str) -> bool:
        t = self.tasks.get(tid)
        if not t or t.status in ("done", "error", "cancelled"):
            return False
        t.cancel.set()
        return True

    # ---------- 执行 ----------

    def _run(self, t: Task) -> None:
        tmpdir = TMP / t.id
        tmpdir.mkdir(parents=True, exist_ok=True)
        vpath = tmpdir / "video.m4s"
        apath = tmpdir / "audio.m4s"
        spath: Path | None = None
        try:
            t.status = "waiting"
            t.phase = "排队中"
            self.sem.acquire()
            if t.cancel.is_set():
                raise Cancelled()
            if not FFMPEG:
                raise DownloadError("找不到 ffmpeg，请把 ffmpeg 放到 bin 目录下")

            t.status = "parsing"
            t.phase = "获取播放地址"
            info = CLIENT.playurl(t.bvid, t.cid, t.qn)
            if not info.get("is_dash"):
                raise DownloadError("该稿件只有单文件流，暂不支持")

            video = None
            for q in info["qualities"]:
                if q["qn"] != t.qn:
                    continue
                for c in q["codecs"]:
                    if c["codecid"] == t.codecid:
                        video = c
                        break
                if video:
                    break
            if not video:
                video = info["qualities"][0]["codecs"][0]
                t.add_log(f"指定清晰度不可用，回退到 {video['name']}")

            audio = info["audio"][0] if info.get("audio") else None
            headers = {"Referer": f"https://www.bilibili.com/video/{t.bvid}"}

            # 视频 0~88%，音频 88~97%，合并 97~100%
            t.phase = "下载视频流"
            t.status = "downloading"

            def cb_v(done: int, total: int, speed: float) -> None:
                t.downloaded = done
                t.total = total
                t.speed = speed
                t.pct = 88.0 * (done / total if total else 0)
                t.eta = (total - done) / speed if speed > 1 else 0

            download_url([video["url"]] + list(video.get("backup") or []), vpath,
                         headers, _Progress(0, cb_v), t.cancel, SETTINGS.get("threads", 8), t.add_log)
            t.add_log(f"视频流完成 {vpath.stat().st_size / 1048576:.1f} MB")

            if audio:
                t.phase = "下载音频流"
                t.pct = 88.0

                def cb_a(done: int, total: int, speed: float) -> None:
                    t.downloaded = done
                    t.total = total
                    t.speed = speed
                    t.pct = 88.0 + 9.0 * (done / total if total else 0)
                    t.eta = (total - done) / speed if speed > 1 else 0

                download_url([audio["url"]] + list(audio.get("backup") or []), apath,
                             headers, _Progress(0, cb_a), t.cancel, 4, t.add_log)
                t.add_log(f"音频流完成 {apath.stat().st_size / 1048576:.1f} MB")

            if t.sub_lan:
                try:
                    t.phase = "下载字幕"
                    for s in CLIENT.subtitles(t.bvid, t.cid):
                        if s["lan"] == t.sub_lan:
                            data = CLIENT.subtitle_json(s["url"])
                            spath = tmpdir / f"sub.{s['lan']}.srt"
                            spath.write_text(json_to_srt(data), encoding="utf-8")
                            t.add_log(f"字幕 {s['label']} 已转换")
                            break
                except Exception as e:
                    t.add_log(f"字幕获取失败（忽略）：{e}")
                    spath = None

            if t.cancel.is_set():
                raise Cancelled()

            t.status = "merging"
            t.phase = "合并音视频"
            t.pct = 97.0
            t.speed = 0
            t.eta = 0

            base = safe_name(t.title)
            if t.page and t.page > 1 or (t.part and t.part not in (t.title, f"P{t.page}")):
                base = f"{base}_P{t.page}_{safe_name(t.part, 40)}"
            out = Path(SETTINGS["out_dir"]) / f"{base}.mp4"
            n = 1
            while out.exists():
                out = Path(SETTINGS["out_dir"]) / f"{base} ({n}).mp4"
                n += 1

            audio_ok = True
            if audio and ("flac" in (audio["url"] or "").lower() or audio.get("id") == 30251):
                audio_ok = False
                t.add_log("Hi-Res 无损音轨，转为 AAC")

            merge(vpath, apath if audio else None, out, FFMPEG, spath, audio_ok)
            dur = duration_of(out, FFMPEG)
            t.file = str(out)
            t.pct = 100.0
            t.status = "done"
            t.phase = "完成"
            t.finished_at = time.time()
            size_mb = out.stat().st_size / 1048576
            t.add_log(f"已保存 {out.name}（{size_mb:.1f} MB，{dur:.0f}s）")
            t.downloaded = int(size_mb * 1048576)

        except Cancelled:
            t.status = "cancelled"
            t.phase = "已取消"
            t.finished_at = time.time()
            t.add_log("任务已取消")
        except (BiliError, DownloadError) as e:
            t.status = "error"
            t.phase = "失败"
            t.error = str(e)
            t.finished_at = time.time()
            t.add_log(f"失败：{e}")
        except Exception as e:
            t.status = "error"
            t.phase = "失败"
            t.error = f"{type(e).__name__}: {e}"
            t.finished_at = time.time()
            t.add_log(traceback.format_exc().splitlines()[-1])
        finally:
            try:
                self.sem.release()
            except ValueError:
                pass
            if t.status in ("done", "error", "cancelled"):
                for p in tmpdir.glob("*"):
                    try:
                        p.unlink()
                    except Exception:
                        pass
                try:
                    tmpdir.rmdir()
                except Exception:
                    pass


MAN = Manager()


# ---------------- HTTP ----------------


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "BiliDown/1.0"

    def log_message(self, fmt, *args):  # 静音访问日志
        pass

    # ---- 工具 ----

    def _json(self, obj, status: int = 200) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception:
            return {}

    def _file(self, path: Path) -> None:
        if not path.exists() or not path.is_file():
            self._json({"error": "not found"}, 404)
            return
        ctype = {
            ".html": "text/html; charset=utf-8",
            ".js": "application/javascript; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".svg": "image/svg+xml",
            ".png": "image/png",
            ".ico": "image/x-icon",
        }.get(path.suffix.lower(), "application/octet-stream")
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    # ---- 路由 ----

    def do_GET(self) -> None:
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            if u.path in ("/", "/index.html"):
                return self._file(WEB / "index.html")
            if u.path.startswith("/static/"):
                rel = u.path[len("/static/"):]
                target = (WEB / rel).resolve()
                if not str(target).startswith(str(WEB.resolve())):
                    return self._json({"error": "forbidden"}, 403)
                return self._file(target)

            if u.path == "/api/me":
                return self._json({**CLIENT.nav(), "ffmpeg": bool(FFMPEG),
                                   "version": "1.0", "out_dir": SETTINGS["out_dir"]})

            if u.path == "/api/login/qr":
                d = CLIENT.qr_generate()
                import qrcode

                img = qrcode.make(d["url"])
                buf = io.BytesIO()
                img.save(buf, format="PNG")
                b64 = base64.b64encode(buf.getvalue()).decode()
                return self._json({"key": d["key"], "image": "data:image/png;base64," + b64})

            if u.path == "/api/login/poll":
                return self._json(CLIENT.qr_poll(q.get("key", "")))

            if u.path == "/api/parse":
                return self._json(self._parse(q.get("url", ""), int(q.get("p") or 0)))

            if u.path == "/api/tasks":
                return self._json({"tasks": MAN.all()})

            if u.path == "/api/settings":
                return self._json(SETTINGS)

            return self._json({"error": "not found"}, 404)
        except BiliError as e:
            return self._json({"error": str(e)}, 400)
        except Exception as e:
            return self._json({"error": f"{type(e).__name__}: {e}"}, 500)

    def do_POST(self) -> None:
        u = urlparse(self.path)
        body = self._body()
        try:
            if u.path == "/api/logout":
                CLIENT.clear_cookie()
                return self._json({"ok": True})

            if u.path == "/api/download":
                return self._json(self._start(body))

            if u.path == "/api/cancel":
                return self._json({"ok": MAN.cancel(body.get("id", ""))})

            if u.path == "/api/open":
                target = body.get("dir") or SETTINGS["out_dir"]
                if not os.path.isdir(target):
                    target = SETTINGS["out_dir"]
                if body.get("file") and os.path.exists(body["file"]):
                    os.startfile(body["file"])
                else:
                    os.startfile(target)
                return self._json({"ok": True})

            if u.path == "/api/settings":
                if body.get("out_dir"):
                    p = Path(body["out_dir"])
                    p.mkdir(parents=True, exist_ok=True)
                    SETTINGS["out_dir"] = str(p)
                if body.get("threads"):
                    SETTINGS["threads"] = max(1, min(16, int(body["threads"])))
                save_settings()
                return self._json(SETTINGS)

            return self._json({"error": "not found"}, 404)
        except BiliError as e:
            return self._json({"error": str(e)}, 400)
        except Exception as e:
            return self._json({"error": f"{type(e).__name__}: {e}"}, 500)

    # ---- 业务 ----

    def _parse(self, text: str, page_hint: int = 0) -> dict:
        bvid, page = CLIENT.resolve_input(text)
        if page_hint:
            page = page_hint
        info = CLIENT.video_info(bvid)
        pages = info["pages"] or [{"cid": None, "page": 1, "part": "", "duration": info["duration"]}]
        page = min(max(1, page), len(pages))
        cur = pages[page - 1]

        play = CLIENT.playurl(bvid, cur["cid"], 127)
        subs = []
        try:
            subs = CLIENT.subtitles(bvid, cur["cid"])
        except Exception:
            pass

        logged = bool(CLIENT.user)
        warn = None
        if play.get("max_qn", 0) <= 32 and not logged:
            warn = "未登录，只能拿到 480P。扫码登录后可下载视频本身支持的最高清晰度。"

        return {
            "video": info,
            "page": page,
            "cid": cur["cid"],
            "play": play,
            "subtitles": subs,
            "logged_in": logged,
            "warn": warn,
        }

    def _start(self, body: dict) -> dict:
        if not FFMPEG:
            raise BiliError("缺少 ffmpeg，无法合并音视频")
        t = Task(
            id=MAN.new_id(),
            bvid=body["bvid"],
            cid=int(body["cid"]),
            title=body.get("title") or "video",
            part=body.get("part") or "",
            page=int(body.get("page") or 1),
            qn=int(body.get("qn") or 80),
            codecid=int(body.get("codecid") or 7),
            sub_lan=body.get("sub_lan") or None,
        )
        MAN.submit(t)
        return {"id": t.id}


def run(port: int = 8760, open_browser: bool = True) -> None:
    # 启动时先探一次登录态，避免首次解析时误判"未登录"而给出错误提示
    try:
        CLIENT.nav()
    except Exception:
        pass
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    url = f"http://127.0.0.1:{port}/"
    print(f"  BiliDown 已启动: {url}")
    print(f"  下载目录: {SETTINGS['out_dir']}")
    print(f"  ffmpeg: {FFMPEG or '未找到'}")
    print("  按 Ctrl+C 退出")
    if open_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已退出")
        srv.shutdown()
