# 第三方组件与许可

BiliDown 自身代码采用 [MIT](LICENSE)。以下是仓库内 / 发布包内附带的第三方组件，
各自权利归原作者所有，本项目的 MIT 许可不涵盖它们。

| 组件 | 版本 | 许可 | 用途 | 来源 |
| --- | --- | --- | --- | --- |
| nlohmann/json | 3.11.3 | MIT | 解析 B 站接口返回的 JSON，单头文件 `vendor/inc/json.hpp` | https://github.com/nlohmann/json |
| qrcodejs | — | MIT | 生成登录二维码，`vendor/qrcode.min.js` | https://github.com/davidshimjs/qrcodejs |
| Microsoft WebView2 SDK | 1.0.x | 微软 BSD 式许可（全文见 `vendor/webview2/LICENSE.txt`、`NOTICE.txt`） | 界面宿主、wbi 无关的 WebView2 COM 头文件 | https://developer.microsoft.com/microsoft-edge/webview2 |
| WebView2Loader.dll | 同上 | 同上 | 运行时动态加载系统 WebView2 Runtime，随发布包附带 | 同上 |
| FFmpeg | 9.0.2 essentials build | **GPLv3**（全文见发布包内 `LICENSE-FFmpeg.txt`） | 把分离的 DASH 音视频流 `-c copy` 封装成 mp4、写入软字幕 | https://www.gyan.dev/ffmpeg/builds/ 、源码 https://ffmpeg.org/download.html |

## 说明

- **FFmpeg 只在 Release 二进制压缩包里附带**，源码仓库不含它，BiliDown 也没有链接任何
  FFmpeg 库：主程序通过命令行调用一个独立的 `ffmpeg.exe` 进程，属于 GPL 意义上的聚合分发
  （mere aggregation），不改变本项目自身代码的 MIT 许可。该二进制来自 gyan.dev 的官方
  Windows 构建，完整对应源码与构建信息可从上面两个地址获取，许可全文随包附带。
  若你不接受 GPLv3 条款，删除包内的 `ffmpeg.exe` 即可，程序其余部分不受影响
  （只是没有音视频合流功能）。
- **WebView2 Runtime** 由 Windows 10/11 系统提供，本仓库与发布包都不分发它；
  随包附带的只是微软允许再分发的 Loader。
- 发布包内另附 `LICENSE-WebView2.txt`（微软许可全文）。
