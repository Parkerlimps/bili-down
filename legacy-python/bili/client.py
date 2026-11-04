"""B 站 web 接口客户端。

只依赖 requests。负责：wbi 签名、视频信息解析、播放地址获取、字幕、扫码登录。
所有请求都走同一个 Session，方便带上登录 cookie。
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import time
import urllib.parse
from typing import Any

import requests

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)

API = "https://api.bilibili.com"
PASSPORT = "https://passport.bilibili.com"

# wbi 签名用的字符重排表（B 站前端硬编码）
MIXIN_TAB = [
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
    33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40,
    61, 26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11,
    36, 20, 34, 44, 52,
]

# qn -> 清晰度名称
QUALITY_NAMES = {
    127: "8K 超高清",
    126: "杜比视界",
    125: "HDR 真彩",
    120: "4K 超清",
    116: "1080P 60帧",
    112: "1080P 高码率",
    80: "1080P 高清",
    74: "720P 60帧",
    64: "720P 高清",
    32: "480P 清晰",
    16: "360P 流畅",
}

CODEC_NAMES = {7: "AVC/H.264", 12: "HEVC/H.265", 13: "AV1"}

ERR_MSG = {
    -101: "账号未登录",
    -400: "请求参数错误",
    -403: "访问权限不足（该视频可能限制地区或需要登录）",
    -404: "视频不存在或已被删除",
    62002: "稿件不可见（可能已失效或设为私密）",
    62004: "稿件审核中",
    87007: "该视频为充电专属内容",
    87008: "该视频为充电专属内容",
}


class BiliError(Exception):
    """带用户可读信息的接口异常。"""


def _clean_name(name: str) -> str:
    """去掉文件名里的非法字符。"""
    name = re.sub(r'[\\/:*?"<>|\r\n\t]', " ", name)
    name = re.sub(r"\s+", " ", name).strip()
    return name[:110]


class BiliClient:
    def __init__(self, cookie_file: str | None = None):
        self.cookie_file = cookie_file
        self.s = requests.Session()
        # 本地直连 B 站更快；环境里若有全局代理反而容易超时
        self.s.trust_env = False
        self.s.headers.update(
            {
                "User-Agent": UA,
                "Referer": "https://www.bilibili.com/",
                "Origin": "https://www.bilibili.com",
                "Accept-Language": "zh-CN,zh;q=0.9",
            }
        )
        self._mixin_key: str | None = None
        self._mixin_ts = 0.0
        self.user: dict[str, Any] | None = None
        if cookie_file and os.path.exists(cookie_file):
            self._load_cookie()

    # ---------- cookie ----------

    def _load_cookie(self) -> None:
        try:
            with open(self.cookie_file, "r", encoding="utf-8") as f:
                data = json.load(f)
            for k, v in (data.get("cookies") or {}).items():
                self.s.cookies.set(k, v, domain=".bilibili.com")
        except Exception:
            pass

    def save_cookie(self) -> None:
        if not self.cookie_file:
            return
        cookies = {c.name: c.value for c in self.s.cookies if c.domain.endswith("bilibili.com")}
        with open(self.cookie_file, "w", encoding="utf-8") as f:
            json.dump({"cookies": cookies, "saved_at": int(time.time())}, f, ensure_ascii=False, indent=2)

    def clear_cookie(self) -> None:
        self.s.cookies.clear()
        self.user = None
        if self.cookie_file and os.path.exists(self.cookie_file):
            os.remove(self.cookie_file)

    # ---------- wbi 签名 ----------

    def _fetch_mixin_key(self) -> str:
        r = self.s.get(f"{API}/x/web-interface/nav", timeout=10).json()
        wbi = (r.get("data") or {}).get("wbi_img") or {}
        if not wbi:
            raise BiliError("获取 wbi 签名密钥失败，请检查网络")
        img = wbi["img_url"].rsplit("/", 1)[1].split(".")[0]
        sub = wbi["sub_url"].rsplit("/", 1)[1].split(".")[0]
        orig = img + sub
        return "".join(orig[i] for i in MIXIN_TAB)[:32]

    def _sign(self, params: dict[str, Any], force: bool = False) -> dict[str, Any]:
        if force or not self._mixin_key or time.time() - self._mixin_ts > 3600:
            self._mixin_key = self._fetch_mixin_key()
            self._mixin_ts = time.time()
        p = {k: v for k, v in params.items() if v is not None}
        p["wts"] = int(time.time())
        p = {
            k: "".join(ch for ch in str(v) if ch not in "!'()*")
            for k, v in p.items()
        }
        query = urllib.parse.urlencode(sorted(p.items()))
        p["w_rid"] = hashlib.md5((query + self._mixin_key).encode()).hexdigest()
        return p

    # ---------- 基础请求 ----------

    def _get(self, url: str, params: dict | None = None, wbi: bool = False,
             referer: str | None = None, retries: int = 2,
             allow: tuple[int, ...] = ()) -> dict:
        """allow 里的业务码不视为错误（例如 nav 未登录返回 -101）。"""
        last = None
        for attempt in range(retries + 1):
            try:
                if wbi:
                    params = self._sign(params or {}, force=attempt > 0)
                kw = {"timeout": 12}
                if referer:
                    kw["headers"] = {"Referer": referer}
                r = self.s.get(url, params=params, **kw)
                if r.status_code == 412:
                    raise BiliError("触发风控（412），请稍后再试")
                r.raise_for_status()
                data = r.json()
                code = data.get("code", 0)
                if code != 0 and code not in allow:
                    raise BiliError(ERR_MSG.get(code, data.get("message") or f"接口错误 {code}"))
                return data
            except BiliError:
                raise
            except Exception as e:  # 网络抖动
                last = e
                if attempt < retries:
                    time.sleep(0.8 * (attempt + 1))
        raise BiliError(f"请求失败：{last}")

    # ---------- 账号 ----------

    def nav(self) -> dict[str, Any]:
        d = self._get(f"{API}/x/web-interface/nav", allow=(-101,)).get("data") or {}
        if d.get("isLogin"):
            self.user = {
                "mid": d.get("mid"),
                "name": d.get("uname"),
                "face": d.get("face"),
                "vip": bool((d.get("vipStatus") or 0) == 1),
                "level": (d.get("level_info") or {}).get("current_level"),
            }
        else:
            self.user = None
        return {"logged_in": bool(self.user), "user": self.user}

    # ---------- 扫码登录 ----------

    def qr_generate(self) -> dict[str, str]:
        d = self._get(f"{PASSPORT}/x/passport-login/web/qrcode/generate").get("data") or {}
        return {"url": d.get("url", ""), "key": d.get("qrcode_key", "")}

    def qr_poll(self, key: str) -> dict[str, Any]:
        """返回 state: waiting / scanned / expired / ok。"""
        r = self.s.get(
            f"{PASSPORT}/x/passport-login/web/qrcode/poll",
            params={"qrcode_key": key, "source": "main-fe-header"},
            timeout=10,
        )
        data = r.json()
        code = (data.get("data") or {}).get("code")
        if code == 0:
            self.save_cookie()
            self.nav()
            return {"state": "ok", "user": self.user}
        if code == 86090:
            return {"state": "scanned"}
        if code == 86038:
            return {"state": "expired"}
        return {"state": "waiting"}

    # ---------- 链接解析 ----------

    BV_RE = re.compile(r"(BV[0-9A-Za-z]{10})")
    AV_RE = re.compile(r"av(\d+)", re.I)

    def resolve_input(self, text: str) -> tuple[str, int]:
        """把用户输入（链接/BV号/av号/短链）解析成 (bvid, 分P序号)。"""
        text = (text or "").strip()
        if not text:
            raise BiliError("请输入视频链接或 BV 号")

        page = 1
        m = re.search(r"[?&]p=(\d+)", text)
        if m:
            page = max(1, int(m.group(1)))

        m = self.BV_RE.search(text)
        if m:
            return m.group(1), page

        if "b23.tv" in text or "bili2233" in text:
            url = text if text.startswith("http") else "https://" + text
            r = self.s.get(url, allow_redirects=True, timeout=12, stream=True)
            final = r.url
            r.close()
            m = self.BV_RE.search(final)
            if m:
                pm = re.search(r"[?&]p=(\d+)", final)
                return m.group(1), int(pm.group(1)) if pm else page
            raise BiliError("短链解析失败，请直接粘贴 BV 号")

        m = self.AV_RE.search(text)
        if m:
            d = self._get(f"{API}/x/web-interface/view", {"aid": m.group(1)}).get("data") or {}
            return d.get("bvid", ""), page

        if "bangumi" in text:
            raise BiliError("暂不支持番剧/影视链接，目前只处理 UP 主普通投稿")

        raise BiliError("无法识别链接，请粘贴 bilibili.com/video/BV... 形式的地址")

    # ---------- 视频信息 ----------

    def video_info(self, bvid: str) -> dict[str, Any]:
        d = self._get(
            f"{API}/x/web-interface/view", {"bvid": bvid},
            referer=f"https://www.bilibili.com/video/{bvid}",
        ).get("data") or {}
        pages = [
            {
                "cid": p.get("cid"),
                "page": p.get("page"),
                "part": p.get("part") or f"P{p.get('page')}",
                "duration": p.get("duration"),
            }
            for p in (d.get("pages") or [])
        ]
        return {
            "bvid": d.get("bvid"),
            "aid": d.get("aid"),
            "title": d.get("title"),
            "cover": d.get("pic"),
            "desc": (d.get("desc") or "")[:300],
            "duration": d.get("duration"),
            "pubdate": d.get("pubdate"),
            "owner": (d.get("owner") or {}).get("name"),
            "owner_face": (d.get("owner") or {}).get("face"),
            "stat": {
                "view": (d.get("stat") or {}).get("view"),
                "like": (d.get("stat") or {}).get("like"),
            },
            "pages": pages,
        }

    def playurl(self, bvid: str, cid: int, qn: int = 127) -> dict[str, Any]:
        """拿 DASH 播放信息，返回整理后的清晰度列表。"""
        d = self._get(
            f"{API}/x/player/wbi/playurl",
            {"bvid": bvid, "cid": cid, "qn": qn, "fnval": 4048, "fnver": 0, "fourk": 1},
            wbi=True,
            referer=f"https://www.bilibili.com/video/{bvid}",
        ).get("data") or {}

        dash = d.get("dash") or {}
        videos = dash.get("video") or []
        if not videos and d.get("durl"):
            # 老视频/部分稿件只有单文件流
            return {
                "is_dash": False,
                "qualities": [{
                    "qn": d.get("quality", 32),
                    "name": QUALITY_NAMES.get(d.get("quality", 32), f"qn{d.get('quality')}"),
                    "codecs": [{"codecid": 7, "name": "单文件", "size": sum(x.get("size", 0) for x in d["durl"])}],
                }],
                "durl": d["durl"],
                "accept": d.get("accept_quality") or [],
            }

        by_qn: dict[int, dict] = {}
        for v in videos:
            qn_id = v.get("id")
            entry = by_qn.setdefault(qn_id, {
                "qn": qn_id,
                "name": QUALITY_NAMES.get(qn_id, f"qn{qn_id}"),
                "width": v.get("width"),
                "height": v.get("height"),
                "fps": v.get("frameRate"),
                "codecs": [],
            })
            entry["codecs"].append({
                "codecid": v.get("codecid"),
                "name": CODEC_NAMES.get(v.get("codecid"), str(v.get("codecid"))),
                "size": v.get("size") or 0,
                "bandwidth": v.get("bandwidth"),
                "url": v.get("baseUrl") or v.get("base_url"),
                "backup": v.get("backupUrl") or v.get("backup_url") or [],
            })

        audio = []
        for a in (dash.get("audio") or []) + ([dash["dolby"]["audio"][0]] if dash.get("dolby", {}).get("audio") else []):
            audio.append({
                "id": a.get("id"),
                "name": {30280: "192K", 30232: "132K", 30216: "64K"}.get(a.get("id"), str(a.get("id"))),
                "bandwidth": a.get("bandwidth"),
                "url": a.get("baseUrl") or a.get("base_url"),
                "backup": a.get("backupUrl") or a.get("backup_url") or [],
            })
        audio.sort(key=lambda x: x.get("bandwidth") or 0, reverse=True)

        qualities = sorted(by_qn.values(), key=lambda x: x["qn"], reverse=True)
        for q in qualities:
            q["codecs"].sort(key=lambda c: (c["codecid"] != 7, c["codecid"]))  # AVC 优先
        return {
            "is_dash": True,
            "qualities": qualities,
            "audio": audio,
            "accept": d.get("accept_quality") or [],
            "max_qn": max((q["qn"] for q in qualities), default=0),
        }

    def subtitles(self, bvid: str, cid: int) -> list[dict[str, Any]]:
        d = self._get(
            f"{API}/x/player/wbi/v2", {"bvid": bvid, "cid": cid}, wbi=True,
            referer=f"https://www.bilibili.com/video/{bvid}",
        ).get("data") or {}
        out = []
        for s in (d.get("subtitle") or {}).get("subtitles") or []:
            url = s.get("subtitle_url") or ""
            if url.startswith("//"):
                url = "https:" + url
            out.append({"lan": s.get("lan"), "label": s.get("lan_doc"), "url": url})
        return out

    def subtitle_json(self, url: str) -> dict:
        r = self.s.get(url, timeout=12, headers={"Referer": "https://www.bilibili.com/"})
        r.raise_for_status()
        return r.json()
