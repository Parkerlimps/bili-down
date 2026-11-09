"""BiliDown 构建脚本。

流程：生成图标 -> 内联前端 -> 编译资源 -> 编译链接 -> 输出到 dist/
用法：python build.py            仅构建
      python build.py --release  构建后按白名单生成发布 zip

仓库不含 MinGW-w64：优先用 toolchain/mingw64/bin/ 下的 g++ / windres，
找不到就回退到 PATH 中已有的 MinGW-w64。
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
DIST = ROOT / "dist"
SRC = ROOT / "src"
TOOLCHAIN = ROOT / "toolchain" / "mingw64" / "bin"


def find_tools() -> tuple[str, str]:
    gxx = TOOLCHAIN / "g++.exe"
    windres = TOOLCHAIN / "windres.exe"
    if gxx.exists() and windres.exists():
        return str(gxx), str(windres)
    g, w = shutil.which("g++"), shutil.which("windres")
    if g and w:
        return g, w
    raise SystemExit(
        "缺少 MinGW-w64：请把 g++.exe / windres.exe 放到 toolchain/mingw64/bin/，"
        "或把它们所在目录加入 PATH"
    )


def make_icon() -> None:
    from PIL import Image, ImageDraw

    size = 256
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8, 8, size - 8, size - 8], radius=52, fill=(37, 99, 235, 255))
    # 下载箭头
    cx = size // 2
    d.rounded_rectangle([cx - 13, 62, cx + 13, 132], radius=6, fill=(255, 255, 255, 255))
    d.polygon([(cx - 40, 118), (cx + 40, 118), (cx, 168)], fill=(255, 255, 255, 255))
    # 底座
    d.rounded_rectangle([70, 180, size - 70, 194], radius=7, fill=(255, 255, 255, 255))

    BUILD.mkdir(exist_ok=True)
    img.save(BUILD / "app.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


def inline_ui() -> None:
    html = (ROOT / "ui" / "index.html").read_text("utf-8")
    css = (ROOT / "ui" / "style.css").read_text("utf-8")
    app = (ROOT / "ui" / "app.js").read_text("utf-8")
    qr = (ROOT / "vendor" / "qrcode.min.js").read_text("utf-8")

    for blob, name in ((app, "app.js"), (qr, "qrcode.js")):
        if "</script" in blob.lower():
            raise SystemExit(f"{name} 含有 </script>，无法内联")

    html = html.replace('<link rel="stylesheet" href="style.css">', "<style>\n" + css + "\n</style>")
    html = html.replace('<script src="qrcode.js"></script>', "<script>\n" + qr + "\n</script>")
    html = html.replace('<script src="app.js"></script>', "<script>\n" + app + "\n</script>")

    BUILD.mkdir(exist_ok=True)
    (BUILD / "ui.html").write_text(html, "utf-8")
    print(f"  前端打包 {len(html.encode()) / 1024:.0f} KB")


def run(cmd: list[str], what: str) -> None:
    print(" ", what)
    # 固定 cwd：resource.rc 里写的 build\ui.html 是相对路径，
    # 从别的目录调用 build.py 时 windres 会找不到
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=ROOT)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit(f"{what} 失败")
    if p.stderr.strip() and "warning" not in p.stderr.lower():
        print(p.stderr[:800])


def app_version() -> str:
    m = re.search(r'constexpr\s+const\s+char\*\s+VERSION\s*=\s*"([^"]+)"',
                  (SRC / "app.h").read_text("utf-8"))
    if not m:
        raise SystemExit("src/app.h 里找不到 VERSION 常量")
    return m.group(1)


def build() -> None:
    gxx, windres = find_tools()

    print("[1/4] 生成图标与前端")
    make_icon()
    inline_ui()

    print("[2/4] 编译资源")
    run([windres, "-I", str(SRC), str(SRC / "resource.rc"), "-o", str(BUILD / "resource.o")],
        "windres")

    print("[3/4] 编译链接")
    cmd = [
        gxx, "-std=c++17", "-O2", "-municode", "-mwindows",
        "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN",
        "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00",
        "-I", str(SRC), "-I", str(ROOT / "vendor" / "inc"),
        "-I", str(ROOT / "vendor" / "webview2" / "build" / "native" / "include"),
        str(SRC / "main.cpp"), str(BUILD / "resource.o"),
        "-o", str(BUILD / "BiliDown.exe"),
        "-lwinhttp", "-lole32", "-loleaut32", "-luuid", "-lshell32",
        "-lgdi32", "-lcomdlg32", "-ladvapi32", "-luser32",
        "-static", "-static-libgcc", "-static-libstdc++", "-s",
    ]
    run(cmd, "g++")

    print("[4/4] 打包")
    DIST.mkdir(exist_ok=True)
    shutil.copy2(BUILD / "BiliDown.exe", DIST / "BiliDown.exe")
    shutil.copy2(ROOT / "vendor" / "webview2" / "build" / "native" / "x64" / "WebView2Loader.dll",
                 DIST / "WebView2Loader.dll")
    ff = ROOT / "bin" / "ffmpeg-9.0.2-essentials_build" / "bin" / "ffmpeg.exe"
    if ff.exists():
        shutil.copy2(ff, DIST / "ffmpeg.exe")
    else:
        print("  未找到 ffmpeg.exe，跳过（下载后不会有音视频合流）")

    exe = DIST / "BiliDown.exe"
    print(f"\n完成：{exe}")
    print(f"  exe {exe.stat().st_size / 1048576:.2f} MB")
    print(f"  目录：{[p.name for p in DIST.iterdir()]}")


def package_release() -> Path:
    """按固定白名单打发布包：dist/ 里还有登录票据、缓存、下载视频，绝不能整个目录打包。"""
    version = app_version()
    files = {
        "BiliDown.exe": DIST / "BiliDown.exe",
        "WebView2Loader.dll": DIST / "WebView2Loader.dll",
        "ffmpeg.exe": DIST / "ffmpeg.exe",
        "LICENSE.txt": ROOT / "LICENSE",
        "THIRD-PARTY-NOTICES.md": ROOT / "THIRD-PARTY-NOTICES.md",
        "LICENSE-FFmpeg.txt": ROOT / "bin" / "ffmpeg-9.0.2-essentials_build" / "LICENSE",
        "LICENSE-WebView2.txt": ROOT / "vendor" / "webview2" / "LICENSE.txt",
    }
    missing = [name for name, path in files.items() if not path.exists()]
    if missing:
        raise SystemExit("发布包缺少文件：" + "、".join(missing))

    readme = f"""BiliDown v{version}

把 BV 号或 B 站视频链接粘进输入框就能下载。

1. 三个 exe/dll 保持在同一目录，双击 BiliDown.exe。
2. 未登录只能拿 480P；点右上角「扫码登录」后可下 1080P / 4K。
   登录票据存在 %LOCALAPPDATA%\\BiliDown\\cookie.json，只在本机，不上传。
3. 视频默认保存在 exe 旁边的 download\\ 目录。点界面「任务」区上方的「更改」
   可自定义下载目录，设置保存在 %LOCALAPPDATA%\\BiliDown\\settings.json，
   重启后仍然有效；「恢复默认」可清掉设置。

ffmpeg.exe 为 GPLv3 软件，许可全文见 LICENSE-FFmpeg.txt，源码与构建信息见
THIRD-PARTY-NOTICES.md。BiliDown 自身为 MIT。

仅供个人离线观看已公开发布的视频，请勿再分发，版权归原 UP 主与哔哩哔哩所有。
项目地址：https://github.com/Parkerlimps/bili-down
"""

    out = DIST / "release"
    out.mkdir(exist_ok=True)
    zpath = out / f"BiliDown-v{version}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, path in files.items():
            z.write(path, name)
        z.writestr("README.txt", readme)
    print(f"\n发布包：{zpath}  {zpath.stat().st_size / 1048576:.1f} MB  {len(files) + 1} 个文件")
    return zpath


if __name__ == "__main__":
    build()
    if "--release" in sys.argv[1:]:
        package_release()
