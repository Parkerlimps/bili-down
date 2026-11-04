// 应用层：RPC 分发、任务队列、下载调度
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>

#include "bili.h"
#include "dl.h"
#include "json.hpp"
#include "util.h"

namespace app {

using json = nlohmann::json;

// %LOCALAPPDATA%。票据、WebView2 缓存、下载临时文件都放这里：
// exe 目录可能没有写权限（Program Files），而这些本来也不是随 exe 走的文件
inline std::string localAppDataDir() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        std::wstring w = p;
        CoTaskMemFree(p);
        return util::narrow(w);
    }
    // 极端情况拿不到 Known Folder，退回临时目录
    wchar_t buf[MAX_PATH];
    UINT n = GetTempPathW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH) {
        std::wstring w(buf, n);
        while (!w.empty() && w.back() == L'\\') w.pop_back();
        return util::narrow(w);
    }
    return ".";
}

enum TaskStatus { TS_QUEUED = 0, TS_PARSING, TS_DOWNLOAD, TS_MERGE, TS_DONE, TS_ERROR, TS_CANCELLED };

inline const char* statusText(int s) {
    switch (s) {
        case TS_QUEUED: return "queued";
        case TS_PARSING: return "parsing";
        case TS_DOWNLOAD: return "downloading";
        case TS_MERGE: return "merging";
        case TS_DONE: return "done";
        case TS_ERROR: return "error";
        case TS_CANCELLED: return "cancelled";
        default: return "unknown";
    }
}

struct Task {
    std::string id, bvid, title, part;
    int page = 1;
    int qn = 80;
    int codecid = 7;
    std::string subLan;

    std::atomic<int> status{TS_QUEUED};
    std::atomic<unsigned long long> done{0};
    std::atomic<unsigned long long> total{0};
    std::atomic<double> pct{0};

    std::mutex mtx;
    std::string phase = "排队中";
    std::string file;
    std::string error;
    double speed = 0;
    double base = 0, span = 88;
    std::vector<std::string> log;
    dl::Progress prog;
    std::thread th;

    void say(const std::string& m) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[16];
        snprintf(buf, sizeof(buf), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        std::lock_guard<std::mutex> lk(mtx);
        log.push_back(buf + m);
        if (log.size() > 60) log.erase(log.begin());
    }

    json toJson() {
        std::lock_guard<std::mutex> lk(mtx);
        json j;
        j["id"] = id;
        j["title"] = title;
        j["part"] = part;
        j["page"] = page;
        j["qnName"] = bili::qualityName(qn);
        j["status"] = statusText(status.load());
        j["phase"] = phase;
        j["done"] = done.load();
        j["total"] = total.load();
        j["speed"] = speed;
        j["pct"] = pct.load();
        j["file"] = file;
        j["error"] = error;
        j["log"] = log;
        return j;
    }
};

class App {
public:
    std::string exeDir;
    std::string dataDir;   // %LOCALAPPDATA%\BiliDown，票据/缓存/临时文件
    std::string outDir;
    std::string ffmpeg;
    std::unique_ptr<bili::Client> client;
    std::vector<std::shared_ptr<Task>> tasks;
    std::mutex mtx;
    int seq = 0;

    void init(const std::string& dir) {
        exeDir = dir;

        dataDir = localAppDataDir() + "\\BiliDown";
        util::makeDirs(dataDir);
        util::makeDirs(dataDir + "\\tmp");

        // 旧版本把 cookie.json 放在 exe 旁，迁移过来，升级后不丢登录态。
        // 只复制不删除，exe 目录里的旧文件留作备份
        std::string newCookie = dataDir + "\\cookie.json";
        std::string oldCookie = exeDir + "\\cookie.json";
        if (!util::fileExists(newCookie) && util::fileExists(oldCookie))
            CopyFileW(util::widen(oldCookie).c_str(), util::widen(newCookie).c_str(), FALSE);

        // 下载产物优先放 exe 旁（用户找得到）；写不进去就退回应用数据目录
        outDir = exeDir + "\\download";
        util::makeDirs(outDir);
        if (!util::dirExists(outDir) || !util::canWrite(outDir))
            outDir = dataDir + "\\download";
        util::makeDirs(outDir);

        ffmpeg = dl::findFfmpeg(dir);
        client = std::make_unique<bili::Client>(newCookie);
        try {
            client->refreshLogin();
        } catch (...) {}
    }

    // ---------- RPC ----------

    std::string handle(const std::string& raw) {
        json req, res;
        try {
            req = json::parse(raw);
        } catch (...) {
            return R"({"ok":false,"error":"请求格式错误"})";
        }
        std::string id = req.value("id", "");
        std::string method = req.value("method", "");
        json p = req.value("params", json::object());
        res["id"] = id;
        try {
            res["ok"] = true;
            res["data"] = dispatch(method, p);
        } catch (const bili::BiliError& e) {
            res["ok"] = false;
            res["error"] = e.what();
        } catch (const std::exception& e) {
            res["ok"] = false;
            res["error"] = std::string("内部错误：") + e.what();
        }
        return res.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    json dispatch(const std::string& m, const json& p) {
        if (m == "init") return initInfo();
        if (m == "parse") return parse(p.value("url", ""), p.value("page", 0));
        if (m == "download") return startDownload(p);
        if (m == "tasks") return listTasks();
        if (m == "cancel") return cancelTask(p.value("id", ""));
        if (m == "open") return openPath(p.value("file", ""));
        if (m == "login.qr") return client->qrGenerate();
        if (m == "login.poll") return pollLogin(p.value("key", ""));
        if (m == "logout") {
            client->clearCookie();
            return json{{"ok", true}};
        }
        throw bili::BiliError("未知方法: " + m);
    }

    json initInfo() {
        auto acc = client->account();
        json j;
        j["loggedIn"] = acc.loggedIn;
        j["name"] = acc.name;
        j["face"] = acc.face;
        j["vip"] = acc.vip;
        j["ffmpeg"] = !ffmpeg.empty();
        j["outDir"] = outDir;
        j["version"] = "1.0";
        return j;
    }

    json pollLogin(const std::string& key) {
        std::string st = client->qrPoll(key);
        json j;
        j["state"] = st;
        if (st == "ok") {
            auto acc = client->account();
            j["user"] = json{{"name", acc.name}, {"face", acc.face}};
        }
        return j;
    }

    json parse(const std::string& input, int pageHint) {
        auto [bvid, page] = client->resolve(input);
        if (pageHint > 0) page = pageHint;
        auto info = client->videoInfo(bvid);
        if (page < 1) page = 1;
        if (page > (int)info.pages.size()) page = (int)info.pages.size();
        auto& cur = info.pages[page - 1];

        auto play = client->playurl(bvid, cur.cid, 127);
        json subs = json::array();
        try {
            subs = client->subtitles(bvid, cur.cid);
        } catch (...) {}

        json v;
        v["bvid"] = info.bvid;
        v["title"] = info.title;
        std::string cover = info.cover;
        if (cover.rfind("http://", 0) == 0) cover = "https://" + cover.substr(7);
        v["cover"] = cover;
        v["desc"] = info.desc;
        v["owner"] = info.owner;
        v["ownerFace"] = info.ownerFace;
        v["duration"] = info.duration;
        v["view"] = info.view;
        json pages = json::array();
        for (auto& pg : info.pages)
            pages.push_back({{"page", pg.page}, {"part", pg.part}, {"duration", pg.duration}});
        v["pages"] = pages;

        json qs = json::array();
        for (auto& q : play.qualities) {
            json cj = json::array();
            for (auto& c : q.codecs) {
                cj.push_back({{"codecid", c.codecid}, {"name", c.name},
                              {"size", c.size}, {"bandwidth", c.bandwidth}});
            }
            qs.push_back({{"qn", q.qn}, {"name", q.name}, {"width", q.width},
                          {"height", q.height}, {"fps", q.fps}, {"codecs", cj}});
        }
        json aj = json::array();
        for (auto& a : play.audio)
            aj.push_back({{"id", a.id}, {"name", a.name}, {"bandwidth", a.bandwidth}});

        json out;
        out["video"] = v;
        out["page"] = page;
        out["cid"] = cur.cid;
        out["play"] = json{{"dash", play.dash}, {"maxQn", play.maxQn},
                           {"qualities", qs}, {"audio", aj}};
        out["subtitles"] = subs;
        bool loggedIn = client->account().loggedIn;
        out["loggedIn"] = loggedIn;
        if (play.maxQn <= 32 && !loggedIn)
            out["warn"] = "未登录，只能拿到 480P。扫码登录后可下载该视频支持的最高清晰度。";
        else
            out["warn"] = nullptr;
        return out;
    }

    // ---------- 下载 ----------

    std::string newId() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        // 多个 worker 可能同时发起下载，序号必须串起来
        std::lock_guard<std::mutex> lk(mtx);
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d%02d%02d-%d", st.wHour, st.wMinute, st.wSecond, ++seq);
        return buf;
    }

    json startDownload(const json& p) {
        if (ffmpeg.empty()) throw bili::BiliError("缺少 ffmpeg.exe，无法合并音视频");
        auto t = std::make_shared<Task>();
        t->id = newId();
        t->bvid = p.value("bvid", "");
        t->title = p.value("title", "video");
        t->part = p.value("part", "");
        t->page = p.value("page", 1);
        t->qn = p.value("qn", 80);
        t->codecid = p.value("codecid", 7);
        t->subLan = p.value("subLan", "");
        {
            std::lock_guard<std::mutex> lk(mtx);
            tasks.insert(tasks.begin(), t);
            pruneTasksLocked();
        }
        auto self = this;
        t->th = std::thread([self, t]() { self->runTask(t); });
        t->th.detach();
        return json{{"id", t->id}};
    }

    void runTask(std::shared_ptr<Task> t) {
        std::string tmp = dataDir + "\\tmp\\" + t->id;
        util::makeDirs(tmp);
        std::string vpath = tmp + "\\video.m4s";
        std::string apath = tmp + "\\audio.m4s";
        std::string spath;
        std::string outPath;  // ffmpeg 的输出，失败/取消时要清掉半成品

        auto setPhase = [&](int st, const char* ph, double base, double span) {
            t->status = st;
            {
                std::lock_guard<std::mutex> lk(t->mtx);
                t->phase = ph;
            }
            t->base = base;
            t->span = span;
        };

        // 网络请求本身没法中断，只能在阶段之间响应取消
        auto checkCancel = [&]() {
            if (t->prog.cancelled) throw bili::BiliError("已取消");
        };

        try {
            checkCancel();
            setPhase(TS_PARSING, "获取播放地址", 0, 0);
            auto info = client->videoInfo(t->bvid);
            checkCancel();
            int pg = std::min(std::max(1, t->page), (int)info.pages.size());
            long long cid = info.pages[pg - 1].cid;

            auto play = client->playurl(t->bvid, cid, t->qn);
            checkCancel();
            if (!play.dash) throw bili::BiliError("该稿件只有单文件流，暂不支持");

            bili::Codec* vc = nullptr;
            for (auto& q : play.qualities) {
                if (q.qn != t->qn) continue;
                for (auto& c : q.codecs)
                    if (c.codecid == t->codecid) vc = &c;
                if (!vc && !q.codecs.empty()) vc = &q.codecs[0];
                break;
            }
            if (!vc && !play.qualities.empty()) vc = &play.qualities[0].codecs[0];
            if (!vc) throw bili::BiliError("没有可用的视频流");

            std::vector<http::Header> hdrs = client->headers("https://www.bilibili.com/video/" + t->bvid);
            std::vector<std::string> vurls{vc->url};
            for (auto& b : vc->backup) vurls.push_back(b);

            setPhase(TS_DOWNLOAD, "下载视频流", 0, 88);
            auto progV = [&]() {
                unsigned long long d = t->prog.done.load(), tot = t->prog.total.load();
                t->done = d;
                t->total = tot;
                t->pct = tot ? (88.0 * d / tot) : 0;
            };
            // 用临时 Progress 绑定到 task
            dl::Progress& P = t->prog;
            P.done = 0;
            P.total = 0;
            P.cancelled = false;
            std::atomic<bool> stopTicker{false};
            std::thread ticker([&]() {
                double last = util::nowSec();
                unsigned long long lastB = 0;
                while (!stopTicker.load()) {
                    Sleep(300);
                    if (stopTicker.load()) break;
                    double now = util::nowSec();
                    unsigned long long b = P.done.load();
                    double sp = (b - lastB) / std::max(0.2, now - last);
                    {
                        std::lock_guard<std::mutex> lk(t->mtx);
                        t->speed = sp;
                    }
                    lastB = b;
                    last = now;
                    progV();
                }
            });

            bool ok = dl::fetch(vurls, vpath, hdrs, P, 8, [&](const std::string& m) { t->say(m); });
            bool wasCancelled = P.cancelled;
            stopTicker = true;
            ticker.join();
            P.cancelled = false;
            if (!ok) throw bili::BiliError(wasCancelled ? "已取消" : "视频流下载失败");

            std::string audioPath;
            if (!play.audio.empty()) {
                auto& a = play.audio[0];
                std::vector<std::string> aurls{a.url};
                for (auto& b : a.backup) aurls.push_back(b);
                setPhase(TS_DOWNLOAD, "下载音频流", 88, 9);
                P.done = 0;
                P.total = 0;
                std::atomic<bool> stopA{false};
                std::thread tk([&]() {
                    unsigned long long lastB = 0;
                    double last = util::nowSec();
                    while (!stopA.load()) {
                        Sleep(250);
                        if (stopA.load()) break;
                        double now = util::nowSec();
                        unsigned long long b = P.done.load(), tot = P.total.load();
                        {
                            std::lock_guard<std::mutex> lk(t->mtx);
                            t->speed = (b - lastB) / std::max(0.2, now - last);
                        }
                        lastB = b;
                        last = now;
                        t->done = b;
                        t->total = tot;
                        t->pct = tot ? 88.0 + 9.0 * b / tot : 88.0;
                    }
                });
                bool aok = dl::fetch(aurls, apath, hdrs, P, 4, [&](const std::string& m) { t->say(m); });
                bool aCancelled = P.cancelled;
                stopA = true;
                tk.join();
                P.cancelled = false;
                if (!aok) throw bili::BiliError(aCancelled ? "已取消" : "音频流下载失败");
                audioPath = apath;
            }

            if (!t->subLan.empty()) {
                setPhase(TS_MERGE, "下载字幕", 97, 0);
                checkCancel();
                try {
                    auto subs = client->subtitles(t->bvid, cid);
                    for (auto& s : subs) {
                        if (s.value("lan", "") != t->subLan) continue;
                        std::string body = client->subtitleRaw(s.value("url", ""));
                        auto j = json::parse(body);
                        std::string srt = dl::subtitleToSrt(j);
                        // 空字幕不要写文件：merge 的 -map 2:s:0 找不到流会整体失败
                        if (!srt.empty()) {
                            spath = tmp + "\\sub.srt";
                            FILE* f = _wfopen(util::widen(spath).c_str(), L"wb");
                            if (f) {
                                fwrite(srt.data(), 1, srt.size(), f);
                                fclose(f);
                            }
                        }
                        break;
                    }
                } catch (...) {
                    spath.clear();
                }
            }

            setPhase(TS_MERGE, "合并音视频", 97, 3);
            checkCancel();
            std::string base = util::safeName(t->title);
            if (t->page > 1) base += "_P" + std::to_string(t->page) + "_" + util::safeName(t->part, 40);
            std::string out = outDir + "\\" + base + ".mp4";
            for (int n = 2; util::fileExists(out); n++)
                out = outDir + "\\" + base + " (" + std::to_string(n) + ").mp4";
            outPath = out;

            std::string err;
            bool needTranscode = false;
            if (!play.audio.empty() && play.audio[0].id == 30251) needTranscode = true;
            if (!dl::merge(ffmpeg, vpath, audioPath, spath, out, &err, needTranscode,
                           &t->prog.cancelled))
                throw bili::BiliError("合并失败：" + err.substr(0, 200));

            {
                std::lock_guard<std::mutex> lk(t->mtx);
                t->file = out;
                t->phase = "完成";
            }
            t->status = TS_DONE;
            t->pct = 100.0;
            t->speed = 0;
            WIN32_FILE_ATTRIBUTE_DATA fad;
            unsigned long long sz = 0;
            if (GetFileAttributesExW(util::widen(out).c_str(), GetFileExInfoStandard, &fad))
                sz = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            t->done = sz;
            t->say("已保存 " + base + ".mp4（" + util::fmtSize((double)sz) + "）");

        } catch (const bili::BiliError& e) {
            bool cancelled = t->prog.cancelled;
            t->status = cancelled ? TS_CANCELLED : TS_ERROR;
            {
                std::lock_guard<std::mutex> lk(t->mtx);
                t->phase = cancelled ? "已取消" : "失败";
                if (!cancelled) t->error = e.what();
            }
            t->say(std::string(cancelled ? "已取消" : "失败：") + e.what());
        } catch (const std::exception& e) {
            t->status = TS_ERROR;
            {
                std::lock_guard<std::mutex> lk(t->mtx);
                t->phase = "失败";
                t->error = e.what();
            }
            t->say(std::string("异常：") + e.what());
        }

        // 清理临时文件；非成功终态时输出文件也是半成品，一并清掉
        DeleteFileW(util::widen(vpath).c_str());
        DeleteFileW(util::widen(apath).c_str());
        if (!spath.empty()) DeleteFileW(util::widen(spath).c_str());
        if (t->status.load() != TS_DONE && !outPath.empty())
            DeleteFileW(util::widen(outPath).c_str());
        RemoveDirectoryW(util::widen(tmp).c_str());
        t->speed = 0;
    }

    // 终态任务最多保留 30 条，防列表无限增长（前端每 600ms 全量拉一次）。
    // 只裁终态，运行中的不动。调用方必须已持有 mtx。
    void pruneTasksLocked() {
        constexpr int MAX_FINISHED = 30;
        std::vector<std::shared_ptr<Task>> keep;
        keep.reserve(tasks.size());
        int finished = 0;
        for (auto& t : tasks) {  // tasks[0] 是最新的
            int st = t->status.load();
            bool done = (st == TS_DONE || st == TS_ERROR || st == TS_CANCELLED);
            if (!done || finished < MAX_FINISHED) {
                keep.push_back(t);
                if (done) finished++;
            }
        }
        if (keep.size() != tasks.size()) tasks.swap(keep);
    }

    json listTasks() {
        json arr = json::array();
        std::lock_guard<std::mutex> lk(mtx);
        pruneTasksLocked();
        for (auto& t : tasks) arr.push_back(t->toJson());
        return json{{"tasks", arr}};
    }

    json cancelTask(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& t : tasks) {
            if (t->id != id) continue;
            t->prog.cancelled = true;
            return json{{"ok", true}};
        }
        return json{{"ok", false}};
    }

    json openPath(const std::string& file) {
        std::wstring target = util::widen(file.empty() ? outDir : file);
        if (!util::fileExists(file) && !util::dirExists(file)) target = util::widen(outDir);
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return json{{"ok", true}};
    }
};

}  // namespace app
