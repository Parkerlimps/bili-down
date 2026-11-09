// 分块并发下载 + ffmpeg 合流
#pragma once

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "http.h"
#include "util.h"

namespace dl {

struct Progress {
    std::atomic<unsigned long long> done{0};
    std::atomic<unsigned long long> total{0};
    // 必须是 atomic：MinGW 的 volatile 就是标准语义，跨线程写不保证可见，
    // 实测取消标志会被吞（主线程轮询时一直读到 false）
    std::atomic<bool> cancelled{false};
};

inline std::string findFfmpeg(const std::string& exeDir) {
    const char* cands[] = {"ffmpeg.exe", "bin/ffmpeg.exe", "../ffmpeg.exe"};
    for (auto c : cands) {
        std::string p = exeDir + "\\" + c;
        if (util::fileExists(p)) return p;
    }
    // 最后一个：PATH 里找
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) return util::narrow(found);
    return "";
}

// 下载一个文件到 path，返回是否成功
inline bool fetch(const std::vector<std::string>& urls, const std::string& path,
                  const std::vector<http::Header>& headers, Progress& prog, int threads,
                  const std::function<void(const std::string&)>& log) {
    if (urls.empty()) return false;

    // B 站每个流会给一个 baseUrl 加若干 backupUrl。baseUrl 会过期或 403，
    // 逐个探测，用第一个能拿到长度的地址作为起点
    std::vector<std::wstring> wurls;
    for (auto& u : urls)
        if (!u.empty()) wurls.push_back(util::widen(u));
    if (wurls.empty()) return false;

    size_t startIdx = 0;
    unsigned long long total = 0;
    int probeStatus = 0;
    for (size_t i = 0; i < wurls.size(); i++) {
        total = http::probeSize(wurls[i], headers, &probeStatus);
        if (total) {
            startIdx = i;
            break;
        }
    }
    if (!total) {
        if (log) {
            if (probeStatus && probeStatus != 206)
                log("该地址不支持分段下载（HTTP " + std::to_string(probeStatus) + "）");
            else
                log("无法获取文件大小");
        }
        return false;
    }
    prog.total = total;
    if (log) log("大小 " + util::fmtSize((double)total));

    std::wstring wpath = util::widen(path);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (log) log("无法创建文件");
        return false;
    }
    // 预分配
    LARGE_INTEGER sz;
    sz.QuadPart = (LONGLONG)total;
    SetFilePointerEx(h, sz, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);

    const unsigned long long CHUNK = 4ull * 1024 * 1024;
    const unsigned int nchunks = (unsigned int)((total + CHUNK - 1) / CHUNK);
    std::atomic<unsigned int> next{0};
    std::atomic<int> failed{0};

    auto worker = [&]() {
        size_t ui = startIdx;  // 本线程当前使用的镜像地址，失败后轮换
        for (;;) {
            unsigned int idx = next.fetch_add(1);
            if (idx >= nchunks || prog.cancelled) return;
            unsigned long long off = (unsigned long long)idx * CHUNK;
            unsigned long long len = std::min(CHUNK, total - off);

            HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                failed++;
                return;
            }
            unsigned long long got = 0;
            // 单块最多试 8 次；不到齐就换下一个镜像，已写入的部分靠 offset 续传
            for (int attempt = 0; attempt < 8 && got < len && !prog.cancelled; attempt++) {
                unsigned long long part = http::downloadRange(
                    wurls[ui], headers, f, off + got, len - got,
                    [&](unsigned long long n) { prog.done += n; }, &prog.cancelled);
                got += part;
                if (got < len) {
                    if (wurls.size() > 1) ui = (ui + 1) % wurls.size();
                    Sleep(500);
                }
            }
            CloseHandle(f);
            if (got < len && !prog.cancelled) {
                failed++;
                return;
            }
        }
    };

    int n = std::max(1, std::min(threads, (int)nchunks));
    std::vector<std::thread> pool;
    for (int i = 0; i < n; i++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    if (prog.cancelled) return false;
    if (failed.load() > 0) {
        if (log) log("有分块下载失败");
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        unsigned long long size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (size != total) {
            if (log) log("文件大小不符");
            return false;
        }
    }
    return true;
}

// B 站字幕 JSON -> SRT
inline std::string subtitleToSrt(const nlohmann::json& j) {
    std::string out;
    if (!j.contains("body")) return out;
    int i = 0;
    for (auto& item : j["body"]) {
        std::string content = item.value("content", "");
        if (content.empty()) continue;
        double from = item.value("from", 0.0), to = item.value("to", 0.0);
        auto ts = [](double sec) {
            int h = (int)(sec / 3600);
            int m = (int)(fmod(sec, 3600) / 60);
            int s = (int)fmod(sec, 60);
            int ms = (int)lround((sec - floor(sec)) * 1000);
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", h, m, s, ms);
            return std::string(buf);
        };
        out += std::to_string(++i) + "\n" + ts(from) + " --> " + ts(to) + "\n" + content + "\n\n";
    }
    return out;
}

inline bool runProcess(const std::string& cmdline, std::string* errOut,
                       const std::atomic<bool>* cancel = nullptr) {
    std::wstring wcmd = util::widen(cmdline);
    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
    buf.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    // 缓冲开大些：下面用 Peek+短睡轮询，吞吐上限 = 缓冲/轮询间隔
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024)) {
        if (errOut) *errOut = "无法创建管道";
        return false;
    }
    // 读端只给父进程用，不该被子进程继承；写端要当子进程的 stdout，必须保持可继承
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        if (errOut) *errOut = "无法启动进程";
        return false;
    }
    // 读管道用独立线程，但不能用阻塞式 ReadFile：ffmpeg 成功时几乎不输出，
    // 主线程会一直睡在 ReadFile 里，取消标志永远轮不到。
    // 也不能靠 CancelSynchronousIo —— MinGW 的 std::thread::native_handle()
    // 给的是 pthread 伪值（实测 0x1），那个 API 直接 ERROR_INVALID_HANDLE。
    // 所以读线程改用 PeekNamedPipe 非阻塞探数据，靠停止标志退出
    std::string out;
    const size_t MAXKEEP = 8000;
    std::atomic<bool> stopRead{false};
    char tmp[4096];
    auto appendOut = [&](DWORD n) {
        if (out.size() >= MAXKEEP) return;
        size_t take = MAXKEEP - out.size();
        if (take > n) take = n;
        out.append(tmp, take);
    };

    std::thread reader([&] {
        for (;;) {
            if (stopRead.load()) break;
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // 管道断了
            if (avail > 0) {
                DWORD want = (sizeof(tmp) - 1) < avail ? (DWORD)(sizeof(tmp) - 1) : avail;
                DWORD got = 0;
                if (!ReadFile(rd, tmp, want, &got, nullptr) || got == 0) break;
                appendOut(got);
                continue;  // 尽快把管道清空，别让子进程堵在写
            }
            Sleep(1);
        }
    });

    bool cancelled = false;
    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 100);
        if (w == WAIT_OBJECT_0) break;
        if (cancel && *cancel) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            cancelled = true;
            break;
        }
    }
    stopRead = true;
    reader.join();

    // 读线程退出时管道里可能还剩着输出，这里排干
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        DWORD want = (sizeof(tmp) - 1) < avail ? (DWORD)(sizeof(tmp) - 1) : avail;
        DWORD got = 0;
        if (!ReadFile(rd, tmp, want, &got, nullptr) || got == 0) break;
        appendOut(got);
    }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    if (cancelled) {
        if (errOut) *errOut = "已取消";
        return false;
    }
    if (code != 0) {
        if (errOut) *errOut = out.empty() ? "ffmpeg 返回错误" : out.substr(0, 600);
        return false;
    }
    return true;
}

inline bool merge(const std::string& ffmpeg, const std::string& video, const std::string& audio,
                  const std::string& subtitle, const std::string& out, std::string* err,
                  bool audioNeedsTranscode = false, const std::atomic<bool>* cancel = nullptr) {
    std::string cmd = "\"" + ffmpeg + "\" -y -nostdin -hide_banner -loglevel error -i \"" + video + "\"";
    if (!audio.empty()) cmd += " -i \"" + audio + "\"";
    if (!subtitle.empty()) cmd += " -i \"" + subtitle + "\"";
    cmd += " -map 0:v:0";
    if (!audio.empty()) cmd += " -map 1:a:0";
    // 没有音轨时字幕是第 1 个输入，写死 2:s:0 会让 ffmpeg 找不到流、整个合流失败
    if (!subtitle.empty()) cmd += std::string(" -map ") + (audio.empty() ? "1" : "2") + ":s:0";

    if (audioNeedsTranscode) {
        cmd += " -c:v copy -c:a aac -b:a 320k";
    } else {
        cmd += " -c copy";
    }
    if (!subtitle.empty()) cmd += " -c:s mov_text -metadata:s:s:0 language=chi";
    cmd += " -movflags +faststart -f mp4 \"" + out + "\"";
    return runProcess(cmd, err, cancel);
}

}  // namespace dl
