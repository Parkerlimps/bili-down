# BiliDown

把 BV 号粘进去就能下载的 Windows 小工具。C++17 / Win32 实现，界面用 WebView2 承载 HTML，
编译产物是单个不到 1 MB 的 exe（附带 `WebView2Loader.dll` 和 `ffmpeg.exe`）。

## 下载使用

到 [Releases](https://github.com/Parkerlimps/bili-down/releases) 下载 `BiliDown-v1.1.0.zip`，
解压后双击 `BiliDown.exe`。三个文件必须放在同一目录：

| 文件 | 作用 |
| --- | --- |
| `BiliDown.exe` | 主程序 |
| `WebView2Loader.dll` | 加载系统 WebView2 运行时（Win10/11 自带） |
| `ffmpeg.exe` | 把分离的音视频流合并成 mp4 |

下载的文件默认落在 exe 旁边的 `download\` 子目录（如果那个位置没有写权限，比如装在
Program Files，会自动改放到 `%LOCALAPPDATA%\BiliDown\download`）。

**下载目录可以改**：界面「任务」区上方显示当前目录，点「更改」→「浏览…」选文件夹，
或直接粘贴完整路径后保存。设置写入 `%LOCALAPPDATA%\BiliDown\settings.json`，下次启动仍然有效；
「恢复默认」清掉设置。启动时会校验目录是否还存在、是否可写，失效（比如 U 盘拔了）就自动
回退默认目录。已经在下载的任务仍写入原来的目录，不受中途改动影响。
「打开下载目录」和任务里的「打开目录」都指向文件实际所在位置。

## 关于清晰度

B 站对未登录用户只发 480P 的流。想要 1080P / 4K，必须点右上角「扫码登录」，用手机 B 站客户端扫一下。

登录票据保存在 `%LOCALAPPDATA%\BiliDown\cookie.json`，只在本机，不上传。删掉它等于退出登录。
（旧版本存在 exe 同目录，新版启动时会自动迁移过来，旧文件保留作备份。）

能下到多高，取决于三件事：视频本身有没有那个清晰度、你的账号权限（大会员）、以及是否登录。

## 支持的输入

- `BV1xxxxxxxxx` / `av123456`
- `https://www.bilibili.com/video/BV...`
- `b23.tv` 短链
- 带 `?p=2` 的分 P 链接，或在界面上切分 P

暂不支持番剧 / 影视（`/bangumi/play/ep...`），也不支持合集批量下载。

## 自己编译

需要 MinGW-w64（`g++` / `windres`）、Python 3 与 Pillow（生成图标）。

仓库不含编译器。把 MinGW-w64 的 `bin/` 放到 `toolchain/mingw64/bin/`，或者让 `g++`、
`windres` 直接在 PATH 里能找到，然后：

```bash
python build.py
```

脚本会：生成图标 → 把 CSS/JS 内联进 HTML → `windres` 编资源 → `g++` 链接 → 输出到 `dist/`。
`python build.py --release` 会额外按白名单生成 `dist/release/BiliDown-v<版本>.zip`
（只打 exe、dll、ffmpeg 与许可文件，不会把 `dist/` 里的登录票据或下载视频打进去）。
链接的全部是 Windows 自带库（winhttp / ole32 / shell32 等），静态链接运行库，不需要额外 DLL。

`ffmpeg.exe` 不随源码分发。想要合流功能，自行下载 FFmpeg 的 Windows 构建放到
`bin/ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe`，脚本会自动拷进 `dist/`；不放也能编译，
只是下载后没有音视频合流。FFmpeg 采用 GPLv3，细节见
[第三方组件与许可](THIRD-PARTY-NOTICES.md)。

前端源码在 `ui/`，改完直接重跑 `build.py` 即可，不需要重启工具链。

## 实现要点

- **接口**：`x/web-interface/view` 取元信息，`x/player/wbi/playurl` 取 DASH 流。
- **wbi 签名**：从 `nav` 接口拿 `img_url`/`sub_url` 的文件名拼成原始串，按固定的 64 位重排表取前 32 位作为 mixin key，参数按 key 排序、过滤 `!'()*`、拼 `wts`，MD5 得 `w_rid`。实现在 `src/bili.h`，`src/md5.h` 是自带的 MD5。
- **下载**：先 `Range: bytes=0-0` 探测总长，预分配文件，多线程各领 4MB 块 `seek` 写入，失败重试 4 次。见 `src/dl.h`。
- **合流**：`ffmpeg -c copy` 直接封装，不重新编码；字幕转 SRT 后作为软字幕轨道（`mov_text`）塞进去。
- **界面通信**：没有本地 HTTP 服务，前端 `chrome.webview.postMessage` → C++ 处理 → `PostWebMessageAsJson` 回包，RPC 形式。
- **下载目录**：`%LOCALAPPDATA%\BiliDown\settings.json` 保存自定义目录，任务创建时拍一份目录快照；
  文件夹选择对话框由 UI 线程弹（RPC 工作线程通过消息同步请求），避免跨线程 COM 问题。见 `src/app.h`。

## 目录

```
src/           C++ 源码（main / app / bili / dl / http / webview / md5 / util / rpcpool）
ui/            前端（index.html / style.css / app.js）
vendor/        编译所需头文件与 Loader（nlohmann-json、qrcodejs、WebView2 SDK 精简版）
build.py       构建脚本
legacy-python/ 早期 Python 实现，仅作参考，未维护
```

`toolchain/`（MinGW-w64）、`bin/`（FFmpeg）、`build/`、`dist/`（产物、缓存、登录票据、
下载目录设置、下载的视频）都在 `.gitignore` 里，不进仓库。

## 第三方组件与许可

本项目自身代码为 [MIT](LICENSE)；仓库与发布包内附带的 nlohmann/json、qrcodejs、
WebView2 SDK、FFmpeg 各有自己的许可，详见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

## 免责

仅供个人离线观看已公开发布的视频。不要再分发下载的内容，版权归原 UP 主与哔哩哔哩所有。
本项目与哔哩哔哩官方无关。
