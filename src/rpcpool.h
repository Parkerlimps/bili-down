// RPC 工作线程池：把 app::handle 挪离 WebView2 的 UI 线程。
// 结果不能从 worker 直接 PostWebMessageAsJson（那要求调用线程是创建 WebView2 的
// STA 线程），所以转成自定义窗口消息投回 UI 线程，由 WndProc 负责发出去。
#pragma once

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rpc {

// lParam 是 new 出来的 std::string，由 UI 线程负责 delete
constexpr UINT WM_RPC_DONE = WM_APP + 1;

class Pool {
public:
    using Handler = std::function<std::string(const std::string&)>;

    void start(HWND notify, Handler h, int n = 4) {
        hwnd_ = notify;
        handler_ = std::move(h);
        for (int i = 0; i < n; i++)
            workers_.emplace_back([this] { loop(); });
    }

    void post(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) return;
            queue_.push_back(msg);
        }
        cv_.notify_one();
    }

    // join 所有 worker。若有 worker 正在跑长请求会等它结束，
    // 所以调用点放在消息循环退出之后、进程结束之前。
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
        workers_.clear();
    }

    ~Pool() { stop(); }

private:
    void loop() {
        for (;;) {
            std::string job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }

            std::string out;
            try {
                // app::handle 自己会捕获业务异常并返回 JSON，这里只是最后一道防线
                out = handler_(job);
            } catch (...) {
                out = R"({"ok":false,"error":"内部错误"})";
            }

            auto* p = new std::string(std::move(out));
            if (!PostMessageW(hwnd_, WM_RPC_DONE, 0, (LPARAM)p)) delete p;
        }
    }

    HWND hwnd_ = nullptr;
    Handler handler_;   // start() 之后只读，不需要锁
    std::vector<std::thread> workers_;
    std::deque<std::string> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace rpc
