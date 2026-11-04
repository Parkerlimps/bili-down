// WebView2 封装：异步初始化、消息桥、尺寸跟随
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>

#include "WebView2.h"
#include "util.h"

namespace wv {

inline std::wstring lastHr(HRESULT hr) {
    wchar_t buf[64];
    swprintf(buf, 64, L"HRESULT 0x%08lX", (unsigned long)hr);
    return buf;
}

// COM 对象公共引用计数
class RefBase {
public:
    virtual ~RefBase() = default;
    ULONG AddRef() { return (ULONG)++ref_; }
    ULONG Release() {
        LONG r = --ref_;
        if (r == 0) delete this;
        return (ULONG)r;
    }

protected:
    std::atomic<LONG> ref_{1};
};

// 环境创建完成
class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, public RefBase {
public:
    using Fn = std::function<void(HRESULT, ICoreWebView2Environment*)>;
    explicit EnvHandler(Fn fn) : fn_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return RefBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return RefBase::Release(); }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        if (fn_) fn_(hr, env);
        return S_OK;
    }

private:
    Fn fn_;
};

// 控制器创建完成
class ControllerHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, public RefBase {
public:
    using Fn = std::function<void(HRESULT, ICoreWebView2Controller*)>;
    explicit ControllerHandler(Fn fn) : fn_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return RefBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return RefBase::Release(); }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* c) override {
        if (fn_) fn_(hr, c);
        return S_OK;
    }

private:
    Fn fn_;
};

// 页面消息
class MessageHandler : public ICoreWebView2WebMessageReceivedEventHandler, public RefBase {
public:
    using Fn = std::function<void(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)>;
    explicit MessageHandler(Fn fn) : fn_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2WebMessageReceivedEventHandler) {
            *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return RefBase::AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return RefBase::Release(); }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override {
        if (fn_) fn_(sender, args);
        return S_OK;
    }

private:
    Fn fn_;
};

class WebView {
public:
    using MessageFn = std::function<void(const std::string&)>;

    HWND hwnd = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* web = nullptr;
    ICoreWebView2Environment* env = nullptr;
    std::atomic<bool> ready{false};
    std::wstring error;

    void OnMessage(MessageFn fn) { onMessage_ = std::move(fn); }
    void SetOnReady(std::function<void()> fn) { onReady_ = std::move(fn); }
    // 环境/控制器是异步创建的，Init 返回 true 只代表"已发起"。
    // 后续失败通过这个回调通知，调用方负责回到 UI 线程提示用户。
    void SetOnFail(std::function<void(HRESULT)> fn) { onFail_ = std::move(fn); }

    bool Init(HWND h, const std::wstring& userDataDir) {
        hwnd = h;
        HMODULE mod = LoadLibraryW(L"WebView2Loader.dll");
        if (!mod) {
            error = L"缺少 WebView2Loader.dll";
            return false;
        }
        auto create = (HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*))
            GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions");
        if (!create) {
            error = L"WebView2Loader.dll 版本不匹配";
            return false;
        }

        auto* handler = new EnvHandler([this](HRESULT hr, ICoreWebView2Environment* e) {
            if (FAILED(hr) || !e) {
                if (onFail_) onFail_(hr);
                return;
            }
            env = e;
            env->AddRef();
            CreateController();
        });

        HRESULT hr = create(nullptr, userDataDir.c_str(), nullptr, handler);
        handler->Release();
        if (FAILED(hr)) {
            error = L"创建 WebView2 环境失败：" + lastHr(hr);
            return false;
        }
        return true;
    }

    void CreateController() {
        auto* h = new ControllerHandler([this](HRESULT hr, ICoreWebView2Controller* c) {
            if (FAILED(hr) || !c) {
                if (onFail_) onFail_(hr);
                return;
            }
            controller = c;
            controller->AddRef();
            controller->get_CoreWebView2(&web);

            RECT rc;
            GetClientRect(hwnd, &rc);
            controller->put_Bounds(rc);

            ICoreWebView2Settings* st = nullptr;
            if (SUCCEEDED(web->get_Settings(&st)) && st) {
                st->put_AreDefaultContextMenusEnabled(TRUE);
                st->put_IsStatusBarEnabled(FALSE);
                st->Release();
            }

            // 白底，避免加载瞬间闪黑
            ICoreWebView2Controller2* c2 = nullptr;
            if (SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2, (void**)&c2)) && c2) {
                COREWEBVIEW2_COLOR bg{255, 255, 255, 255};
                c2->put_DefaultBackgroundColor(bg);
                c2->Release();
            }

            BindMessage();
            controller->put_IsVisible(TRUE);

            ready = true;
            if (onReady_) onReady_();
        });
        env->CreateCoreWebView2Controller(hwnd, h);
        h->Release();
    }

    void BindMessage() {
        auto* h = new MessageHandler([this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                if (onMessage_) onMessage_(util::narrow(raw));
                CoTaskMemFree(raw);
            }
        });
        EventRegistrationToken tok{};
        web->add_WebMessageReceived(h, &tok);
        h->Release();
    }

    void LoadHtml(const std::string& html) {
        if (web) web->NavigateToString(util::widen(html).c_str());
    }

    void PostJson(const std::string& json) {
        if (web) web->PostWebMessageAsJson(util::widen(json).c_str());
    }

    void Resize() {
        if (!controller) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        controller->put_Bounds(rc);
    }

    // 必须在 CoUninitialize 之前调用：COM 对象不能活过 COM 自身的销毁
    void Close() {
        if (web) { web->Release(); web = nullptr; }
        if (controller) { controller->Release(); controller = nullptr; }
        if (env) { env->Release(); env = nullptr; }
    }

    ~WebView() { Close(); }

private:
    MessageFn onMessage_;
    std::function<void()> onReady_;
    std::function<void(HRESULT)> onFail_;
};

}  // namespace wv
