// BiliDown —— B 站视频下载工具（C++ / Win32 + WebView2）
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "app.h"
#include "resource.h"
#include "rpcpool.h"
#include "util.h"
#include "webview.h"

static app::App g_app;
static wv::WebView g_web;
static rpc::Pool g_rpc;
static std::string g_html;

// WebView2 异步初始化失败的回报消息
constexpr UINT WM_WV_FAIL = WM_APP + 2;
// 初始化超时兜底：有几种失败 WebView2 既不回调成功也不回调失败
constexpr UINT_PTR TIMER_WV_INIT = 1;

static std::string loadHtmlResource() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_HTML), RT_RCDATA);
    if (!res) return "<h1>资源缺失</h1>";
    DWORD size = SizeofResource(nullptr, res);
    HGLOBAL mem = LoadResource(nullptr, res);
    if (!mem || !size) return "<h1>资源读取失败</h1>";
    const char* data = (const char*)LockResource(mem);
    return std::string(data, size);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (g_web.controller) g_web.Resize();
            return 0;
        case WM_GETMINMAXINFO: {
            // 必须先让系统填好默认的 ptMaxSize / ptMaxTrackSize，否则窗口会被压成 0
            DefWindowProcW(hwnd, msg, wp, lp);
            auto* mm = (MINMAXINFO*)lp;
            HDC dc = GetDC(nullptr);
            int dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(nullptr, dc);
            mm->ptMinTrackSize.x = MulDiv(820, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(600, dpi, 96);
            return 0;
        }
        case rpc::WM_RPC_DONE: {
            // worker 线程算完的结果，回到 UI 线程才真正发给页面
            auto* p = (std::string*)lp;
            if (p) {
                g_web.PostJson(*p);
                delete p;
            }
            return 0;
        }
        case WM_WV_FAIL: {
            wchar_t buf[512];
            swprintf(buf, 512,
                     L"WebView2 初始化失败（HRESULT 0x%08lX）。\n\n"
                     L"请确认系统已安装 Microsoft Edge WebView2 Runtime。",
                     (unsigned long)wp);
            MessageBoxW(hwnd, buf, L"BiliDown 启动失败", MB_ICONERROR);
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_WV_INIT) {
                KillTimer(hwnd, TIMER_WV_INIT);
                // 实测：userDataFolder 建不起来时（例如程序装在 Program Files，
                // 目录没有写权限），WebView2 既不走成功回调也不走失败回调，
                // 就这么悬着。没有这道超时，用户看到的就是一片白窗口。
                if (!g_web.ready) {
                    MessageBoxW(hwnd,
                                L"WebView2 在 10 秒内没有完成初始化。\n\n"
                                L"常见原因：\n"
                                L"· 系统缺少 Microsoft Edge WebView2 Runtime\n"
                                L"· 程序所在目录没有写入权限，缓存目录建不起来\n\n"
                                L"建议把程序放到有写权限的目录再试。",
                                L"BiliDown 启动失败", MB_ICONERROR);
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
            FillRect((HDC)wp, &rc, b);
            DeleteObject(b);
            return 1;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

namespace {

// COM 初始化要覆盖所有返回路径（后面有几处提前 return），用 RAII 最稳
struct ComScope {
    bool ok = false;
    ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComScope() { if (ok) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // WebView2 要求创建它的线程是 STA，且这一步必须在建窗口、建环境之前做。
    // 缺了它有些机器上 CreateCoreWebView2EnvironmentWithOptions 会直接失败。
    ComScope com;
    if (!com.ok) {
        MessageBoxW(nullptr, L"COM 初始化失败（该线程无法进入 STA 模式），WebView2 不能启动。",
                    L"BiliDown 启动失败", MB_ICONERROR);
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::string exeDir = util::exeDir();
    g_app.init(exeDir);
    g_html = loadHtmlResource();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(255, 255, 255));
    wc.lpszClassName = L"BiliDownWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    // 按系统 DPI 换算物理尺寸，保证 WebView2 内部逻辑尺寸仍是 1040x800
    HDC screenDc = GetDC(nullptr);
    int dpi = GetDeviceCaps(screenDc, LOGPIXELSX);
    ReleaseDC(nullptr, screenDc);
    int w = MulDiv(1040, dpi, 96), h = MulDiv(780, dpi, 96);
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(0, L"BiliDownWnd", L"BiliDown · B 站视频下载",
                                WS_OVERLAPPEDWINDOW, (sx - w) / 2, std::max(0, (sy - h) / 2 - 30),
                                w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // 页面发来的每条 RPC 都丢进工作线程，UI 线程只负责收发消息，
    // 不再被 parse 那串网络请求堵住
    g_web.OnMessage([](const std::string& msg) {
        g_rpc.post(msg);
    });

    g_web.SetOnReady([hwnd]() {
        KillTimer(hwnd, TIMER_WV_INIT);
        g_web.LoadHtml(g_html);
    });

    // 环境和控制器是异步创建的，Init 早就返回 true 了。失败时只能靠这条消息
    // 回到 UI 线程弹提示，否则用户看到的只是一片白窗口
    g_web.SetOnFail([hwnd](HRESULT hr) {
        PostMessageW(hwnd, WM_WV_FAIL, (WPARAM)hr, 0);
    });

    g_rpc.start(hwnd, [](const std::string& msg) { return g_app.handle(msg); });

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    std::string wvData = g_app.dataDir + "\\webview";
    util::makeDirs(wvData);
    if (!g_web.Init(hwnd, util::widen(wvData))) {
        std::wstring tip = g_web.error.empty() ? L"WebView2 初始化失败" : g_web.error;
        tip += L"\n\n请确认系统已安装 Microsoft Edge WebView2 Runtime。";
        MessageBoxW(hwnd, tip.c_str(), L"BiliDown 启动失败", MB_ICONERROR);
        return 1;
    }
    SetTimer(hwnd, TIMER_WV_INIT, 10000, nullptr);
    if (g_web.ready) KillTimer(hwnd, TIMER_WV_INIT);  // 极快的机器上可能已经就绪

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 先停线程池再让进程结束，否则 worker 可能还在用 g_app / g_web
    g_rpc.stop();
    // 队列里可能还压着没处理的完成消息，里面的字符串要收掉
    MSG left;
    while (PeekMessageW(&left, hwnd, rpc::WM_RPC_DONE, rpc::WM_RPC_DONE, PM_REMOVE))
        delete (std::string*)left.lParam;

    // COM 对象要先于 CoUninitialize 释放（ComScope 要到 return 时才析构）
    g_web.Close();
    return 0;
}
