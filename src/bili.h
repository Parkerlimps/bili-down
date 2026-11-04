// B 站 web 接口：wbi 签名、视频信息、播放地址、字幕、扫码登录
#pragma once

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "http.h"
#include "json.hpp"
#include "md5.h"
#include "util.h"

namespace bili {

using json = nlohmann::json;

constexpr wchar_t API_HOST[] = L"https://api.bilibili.com";
constexpr wchar_t PASSPORT_HOST[] = L"https://passport.bilibili.com";
constexpr wchar_t UA[] =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    L"Chrome/131.0.0.0 Safari/537.36";

// wbi 字符重排表（B 站前端硬编码）
static const int MIXIN_TAB[64] = {
    46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35, 27, 43, 5,  49, 33, 9,
    42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,
    60, 51, 30, 4,  22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52};

struct BiliError : std::runtime_error {
    explicit BiliError(const std::string& m) : std::runtime_error(m) {}
};

inline std::string errText(int code, const std::string& msg) {
    switch (code) {
        case -101: return "账号未登录";
        case -400: return "请求参数错误";
        case -403: return "访问权限不足（该视频可能限制地区或需要登录）";
        case -404: return "视频不存在或已被删除";
        case 62002: return "稿件不可见（可能已失效或设为私密）";
        case 62003: return "稿件审核中";
        case 87007:
        case 87008: return "该视频为充电专属内容";
        default: return msg.empty() ? ("接口错误 " + std::to_string(code)) : msg;
    }
}

struct Page {
    long long cid = 0;
    int page = 1;
    std::string part;
    int duration = 0;
};

struct VideoInfo {
    std::string bvid, title, cover, desc, owner, ownerFace;
    long long aid = 0, view = 0;
    int duration = 0;
    std::vector<Page> pages;
};

struct Codec {
    int codecid = 7;
    std::string name;
    unsigned long long size = 0;
    long long bandwidth = 0;
    std::string url;
    std::vector<std::string> backup;
};

struct Quality {
    int qn = 0;
    std::string name;
    int width = 0, height = 0;
    double fps = 0;
    std::vector<Codec> codecs;
};

struct AudioStream {
    int id = 0;
    std::string name;
    long long bandwidth = 0;
    std::string url;
    std::vector<std::string> backup;
};

struct PlayInfo {
    bool dash = true;
    int maxQn = 0;
    std::vector<Quality> qualities;
    std::vector<AudioStream> audio;
};

inline std::string qualityName(int qn) {
    switch (qn) {
        case 127: return "8K 超高清";
        case 126: return "杜比视界";
        case 125: return "HDR 真彩";
        case 120: return "4K 超清";
        case 116: return "1080P 60帧";
        case 112: return "1080P 高码率";
        case 80: return "1080P 高清";
        case 74: return "720P 60帧";
        case 64: return "720P 高清";
        case 32: return "480P 清晰";
        case 16: return "360P 流畅";
        default: return "qn" + std::to_string(qn);
    }
}

inline std::string codecName(int id) {
    switch (id) {
        case 7: return "AVC/H.264";
        case 12: return "HEVC/H.265";
        case 13: return "AV1";
        default: return std::to_string(id);
    }
}

class Client {
public:
    std::string userName;
    std::string userFace;
    bool loggedIn = false;
    long long mid = 0;
    bool vip = false;
    std::string cookiePath;
    std::string lastError;

    explicit Client(std::string cookieFile) : cookiePath(std::move(cookieFile)) {
        loadCookie();
        detectMixin();
    }

    // ---------- cookie ----------

    void loadCookie() {
        if (!util::fileExists(cookiePath)) return;
        FILE* f = _wfopen(util::widen(cookiePath).c_str(), L"rb");
        if (!f) return;
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        fclose(f);
        try {
            auto j = json::parse(s);
            std::string ck = j.value("cookie", "");
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            cookie = std::move(ck);
        } catch (...) {}
    }

    void saveCookie() {
        std::string ck;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            ck = cookie;
        }
        json j;
        j["cookie"] = ck;
        FILE* f = _wfopen(util::widen(cookiePath).c_str(), L"wb");
        if (!f) return;
        std::string s = j.dump(2);
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }

    void clearCookie() {
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            cookie.clear();
            loggedIn = false;
            userName.clear();
            userFace.clear();
            mid = 0;
            vip = false;
        }
        DeleteFileW(util::widen(cookiePath).c_str());
    }

    std::vector<http::Header> headers(const std::string& referer = "https://www.bilibili.com/") const {
        std::vector<http::Header> h = {
            {L"User-Agent", UA},
            {L"Referer", util::widen(referer)},
            {L"Origin", L"https://www.bilibili.com"},
            {L"Accept", L"application/json, text/plain, */*"},
            {L"Accept-Language", L"zh-CN,zh;q=0.9"},
        };
        // 下载线程会在这里读 cookie，而扫码登录成功时主线程会重建它，
        // 必须先复制到局部变量再转换，不能在锁外直接遍历成员
        std::string ck;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            ck = cookie;
        }
        if (!ck.empty()) h.push_back({L"Cookie", util::widen(ck)});
        return h;
    }

    // ---------- wbi ----------

    void detectMixin() {
        auto r = http::get(std::wstring(API_HOST) + L"/x/web-interface/nav", headers());
        if (r.body.empty()) return;
        try {
            auto j = json::parse(r.body);
            mixinFromNav(j);
        } catch (...) {}
    }

    std::string sign(std::map<std::string, std::string> params) {
        // 整个签名过程持锁。mixin 过期时这里会发一次 nav 请求（约 200ms，一小时一次），
        // 代价是其他线程短暂等待，换来的是不可能有两个线程同时改写 mixinKey_
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        if (mixinKey_.empty() || util::nowSec() - mixinTs_ > 3600) refreshMixinLocked();
        params["wts"] = std::to_string((long long)time(nullptr));
        std::string query;
        for (auto& [k, v] : params) {
            std::string clean;
            for (char c : v) {
                if (c == '!' || c == '\'' || c == '(' || c == ')' || c == '*') continue;
                clean += c;
            }
            if (!query.empty()) query += "&";
            query += util::urlEncode(k) + "=" + util::urlEncode(clean);
        }
        std::string rid = md5::hex(query + mixinKey_);
        return query + "&w_rid=" + rid;
    }

    // ---------- 通用请求 ----------

    json getJson(const std::wstring& url, const std::string& referer, bool allowNotLogin = false) {
        auto r = http::get(url, headers(referer));
        if (r.body.empty()) throw BiliError("网络请求失败（无响应）");
        json j;
        try {
            j = json::parse(r.body);
        } catch (const json::exception&) {
            throw BiliError("接口返回格式异常，可能触发风控");
        }
        int code = j.value("code", 0);
        if (code != 0 && !(allowNotLogin && code == -101))
            throw BiliError(errText(code, j.value("message", "")));
        return j;
    }

    // ---------- 账号 ----------

    struct Account {
        bool loggedIn = false;
        std::string name;
        std::string face;
        long long mid = 0;
        bool vip = false;
    };

    // RPC 现在跑在多个工作线程里，登录态必须整体加锁拷出来，
    // 不能让调用方直接读 loggedIn/userName（那是 std::string，并发读写会崩）
    Account account() const {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        return Account{loggedIn, userName, userFace, mid, vip};
    }

    void refreshLogin() {
        // 网络请求不持锁，只在最后写回时锁一下
        auto j = getJson(std::wstring(API_HOST) + L"/x/web-interface/nav",
                         "https://www.bilibili.com/", true);
        const auto& d = j["data"];
        bool li = d.value("isLogin", false);
        std::string nm, fc;
        long long m = 0;
        bool v = false;
        if (li) {
            nm = d.value("uname", "");
            fc = d.value("face", "");
            m = d.value("mid", 0LL);
            v = d.value("vipStatus", 0) == 1;
        }
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        loggedIn = li;
        userName = std::move(nm);
        userFace = std::move(fc);
        mid = m;
        vip = v;
    }

    json qrGenerate() {
        auto j = getJson(std::wstring(PASSPORT_HOST) + L"/x/passport-login/web/qrcode/generate",
                         "https://www.bilibili.com/");
        json out;
        out["url"] = j["data"].value("url", "");
        out["key"] = j["data"].value("qrcode_key", "");
        return out;
    }

    // state: waiting / scanned / expired / ok
    std::string qrPoll(const std::string& key) {
        std::wstring url = std::wstring(PASSPORT_HOST) +
                           L"/x/passport-login/web/qrcode/poll?qrcode_key=" + util::widen(util::urlEncode(key)) +
                           L"&source=main-fe-header";
        auto r = http::get(url, headers());
        if (r.body.empty()) return "waiting";
        json j;
        try {
            j = json::parse(r.body);
        } catch (...) { return "waiting"; }
        int code = j["data"].value("code", -1);
        if (code == 0) {
            collectCookies(r);
            saveCookie();
            refreshLogin();
            return "ok";
        }
        if (code == 86090) return "scanned";
        if (code == 86038) return "expired";
        return "waiting";
    }

    // 从 Set-Cookie 响应头里提取登录票据
    void collectCookies(const http::Response& r);

    // ---------- 链接解析 ----------

    static bool isBv(const std::string& s) {
        if (s.size() != 12 || s[0] != 'B' || s[1] != 'V') return false;
        // BV 号后 10 位只会是字母数字，挡掉页面文本里的巧合命中
        for (int i = 2; i < 12; i++) {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
                return false;
        }
        return true;
    }

    // 返回 bvid 与分P序号
    std::pair<std::string, int> resolve(const std::string& input) {
        if (input.empty()) throw BiliError("请输入视频链接或 BV 号");
        int page = 1;
        auto findP = [](const std::string& s) {
            size_t q = s.find("p=");
            if (q == std::string::npos) return 0;
            int v = atoi(s.c_str() + q + 2);
            return v > 0 ? v : 0;
        };
        if (int p = findP(input)) page = p;

        size_t bv = input.find("BV");
        while (bv != std::string::npos) {
            std::string cand = input.substr(bv, 12);
            if (isBv(cand)) return {cand, page};
            bv = input.find("BV", bv + 1);
        }

        if (input.find("b23.tv") != std::string::npos ||
            input.find("bili2233") != std::string::npos) {
            std::wstring u = util::widen(input);
            if (u.rfind(L"http", 0) != 0) u = L"https://" + u;

            // 从 30x 响应头里抠出 Location 的值
            auto locationOf = [](const http::Req& r) -> std::wstring {
                std::wstring raw = r.rawHeaders();
                size_t pos = 0;
                while (pos < raw.size()) {
                    size_t eol = raw.find(L"\r\n", pos);
                    std::wstring line =
                        raw.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
                    pos = (eol == std::wstring::npos) ? raw.size() : eol + 2;
                    if (line.size() > 10) {
                        std::wstring head = line.substr(0, 10);
                        for (auto& c : head) c = towlower(c);
                        if (head == L"location:") {
                            std::wstring v = line.substr(10);
                            while (!v.empty() && v.front() == L' ') v.erase(v.begin());
                            return v;
                        }
                    }
                }
                return L"";
            };
            auto bvInWide = [](const std::wstring& s) -> std::string {
                size_t p = s.find(L"BV");
                while (p != std::wstring::npos) {
                    std::string cand = util::narrow(s.substr(p, 12));
                    if (isBv(cand)) return cand;
                    p = s.find(L"BV", p + 1);
                }
                return "";
            };
            auto bvInUtf8 = [](const std::string& s) -> std::string {
                size_t p = s.find("BV");
                while (p != std::string::npos) {
                    std::string cand = s.substr(p, 12);
                    if (isBv(cand)) return cand;
                    p = s.find("BV", p + 1);
                }
                return "";
            };

            // 第一选择：关掉自动跟随，Location 头里就是目标地址，
            // 不会混进视频页推荐位的 BV
            std::string bv;
            http::Req r = http::open(u, headers(), L"GET", false);
            if (r) {
                int st = r.status();
                if (st >= 300 && st < 400) {
                    std::wstring loc = locationOf(r);
                    if (!loc.empty()) {
                        if (loc.rfind(L"http", 0) != 0)
                            loc = L"https://b23.tv" + (loc.rfind(L"/", 0) == 0 ? loc : L"/" + loc);
                        bv = bvInWide(loc);
                    }
                }
            }

            // 兜底：Location 里没有 BV（b23.tv/av123 会跳到 /video/av123 这种），
            // 跟随重定向读页面找。可能命中推荐位，但比直接失败强
            if (bv.empty()) {
                http::Req r2 = http::open(u, headers());
                if (r2) {
                    std::string body;
                    DWORD avail = 0;
                    std::vector<char> buf(65536);
                    while (WinHttpQueryDataAvailable(r2.req, &avail) && avail > 0) {
                        DWORD got = 0;
                        DWORD want = (DWORD)(avail > buf.size() ? buf.size() : avail);
                        if (!WinHttpReadData(r2.req, buf.data(), want, &got) || got == 0) break;
                        body.append(buf.data(), got);
                        if (body.size() > 400000) break;
                    }
                    bv = bvInUtf8(body);
                }
            }

            if (!bv.empty()) return {bv, page};
            throw BiliError("短链解析失败，请直接粘贴 BV 号");
        }

        if (input.find("av") != std::string::npos || input.find("AV") != std::string::npos) {
            size_t p = input.find("av") != std::string::npos ? input.find("av") : input.find("AV");
            long long aid = atoll(input.c_str() + p + 2);
            if (aid > 0) {
                auto j = getJson(std::wstring(API_HOST) + L"/x/web-interface/view?aid=" +
                                     std::to_wstring(aid), "https://www.bilibili.com/");
                return {j["data"].value("bvid", ""), page};
            }
        }

        if (input.find("bangumi") != std::string::npos)
            throw BiliError("暂不支持番剧/影视链接，目前只处理 UP 主普通投稿");

        throw BiliError("无法识别链接，请粘贴 bilibili.com/video/BV... 形式的地址");
    }

    // ---------- 视频 ----------

    VideoInfo videoInfo(const std::string& bvid) {
        auto j = getJson(std::wstring(API_HOST) + L"/x/web-interface/view?bvid=" + util::widen(bvid),
                         "https://www.bilibili.com/video/" + bvid);
        const auto& d = j["data"];
        VideoInfo v;
        v.bvid = d.value("bvid", bvid);
        v.aid = d.value("aid", 0LL);
        v.title = d.value("title", "");
        v.cover = d.value("pic", "");
        v.desc = d.value("desc", "");
        v.duration = d.value("duration", 0);
        if (d.contains("owner")) {
            v.owner = d["owner"].value("name", "");
            v.ownerFace = d["owner"].value("face", "");
        }
        if (d.contains("stat")) v.view = d["stat"].value("view", 0LL);
        if (d.contains("pages")) {
            for (auto& p : d["pages"]) {
                Page pg;
                pg.cid = p.value("cid", 0LL);
                pg.page = p.value("page", 1);
                pg.part = p.value("part", "");
                pg.duration = p.value("duration", 0);
                v.pages.push_back(pg);
            }
        }
        if (v.pages.empty()) v.pages.push_back({d.value("cid", 0LL), 1, "", v.duration});
        return v;
    }

    PlayInfo playurl(const std::string& bvid, long long cid, int qn = 127) {
        std::map<std::string, std::string> p = {
            {"bvid", bvid}, {"cid", std::to_string(cid)}, {"qn", std::to_string(qn)},
            {"fnval", "4048"}, {"fnver", "0"}, {"fourk", "1"}};
        std::wstring url = std::wstring(API_HOST) + L"/x/player/wbi/playurl?" + util::widen(sign(p));
        auto j = getJson(url, "https://www.bilibili.com/video/" + bvid);
        const auto& d = j["data"];

        PlayInfo out;
        if (!d.contains("dash")) {
            out.dash = false;
            out.maxQn = d.value("quality", 32);
            return out;
        }
        const auto& dash = d["dash"];
        std::map<int, Quality> qmap;
        if (dash.contains("video")) {
            for (auto& v : dash["video"]) {
                int id = v.value("id", 0);
                auto it = qmap.find(id);
                if (it == qmap.end()) {
                    Quality q;
                    q.qn = id;
                    q.name = qualityName(id);
                    q.width = v.value("width", 0);
                    q.height = v.value("height", 0);
                    q.fps = atof(v.value("frameRate", "0").c_str());
                    it = qmap.emplace(id, q).first;
                }
                Codec c;
                c.codecid = v.value("codecid", 7);
                c.name = codecName(c.codecid);
                c.bandwidth = v.value("bandwidth", 0LL);
                if (v.contains("size") && v["size"].is_number()) c.size = v["size"].get<unsigned long long>();
                c.url = v.value("baseUrl", v.value("base_url", ""));
                if (v.contains("backupUrl")) {
                    for (auto& b : v["backupUrl"]) c.backup.push_back(b.get<std::string>());
                }
                it->second.codecs.push_back(c);
            }
        }
        for (auto& [qn_, q] : qmap) {
            std::sort(q.codecs.begin(), q.codecs.end(), [](const Codec& a, const Codec& b) {
                if (a.codecid == 7) return true;
                if (b.codecid == 7) return false;
                return a.codecid < b.codecid;
            });
            out.qualities.push_back(q);
            out.maxQn = std::max(out.maxQn, q.qn);
        }
        std::sort(out.qualities.begin(), out.qualities.end(),
                  [](const Quality& a, const Quality& b) { return a.qn > b.qn; });

        if (dash.contains("audio")) {
            for (auto& a : dash["audio"]) {
                AudioStream s;
                s.id = a.value("id", 0);
                s.bandwidth = a.value("bandwidth", 0LL);
                s.name = s.id == 30280 ? "192K" : s.id == 30232 ? "132K" : s.id == 30216 ? "64K" : std::to_string(s.id);
                s.url = a.value("baseUrl", a.value("base_url", ""));
                if (a.contains("backupUrl"))
                    for (auto& b : a["backupUrl"]) s.backup.push_back(b.get<std::string>());
                out.audio.push_back(s);
            }
            std::sort(out.audio.begin(), out.audio.end(),
                      [](const AudioStream& a, const AudioStream& b) { return a.bandwidth > b.bandwidth; });
        }
        return out;
    }

    json subtitles(const std::string& bvid, long long cid) {
        std::map<std::string, std::string> p = {{"bvid", bvid}, {"cid", std::to_string(cid)}};
        std::wstring url = std::wstring(API_HOST) + L"/x/player/wbi/v2?" + util::widen(sign(p));
        auto j = getJson(url, "https://www.bilibili.com/video/" + bvid);
        json out = json::array();
        if (j["data"].contains("subtitle") && j["data"]["subtitle"].contains("subtitles")) {
            for (auto& s : j["data"]["subtitle"]["subtitles"]) {
                std::string u = s.value("subtitle_url", "");
                if (u.rfind("//", 0) == 0) u = "https:" + u;
                out.push_back({{"lan", s.value("lan", "")},
                               {"label", s.value("lan_doc", "")},
                               {"url", u}});
            }
        }
        return out;
    }

    std::string subtitleRaw(const std::string& url) {
        auto r = http::get(util::widen(url), headers());
        return r.body;
    }

private:
    // 下面两个函数直接改写 mixinKey_/mixinTs_，调用前必须已持有 mtx_。
    // 例外是 detectMixin()：它只在构造函数里跑，那时对象还没被别的线程看到。
    void mixinFromNav(const json& j) {
        const auto& d = j["data"];
        if (!d.contains("wbi_img")) return;
        std::string img = d["wbi_img"].value("img_url", "");
        std::string sub = d["wbi_img"].value("sub_url", "");
        auto nameOf = [](const std::string& u) {
            size_t a = u.find_last_of('/');
            std::string f = (a == std::string::npos) ? u : u.substr(a + 1);
            size_t b = f.find('.');
            return (b == std::string::npos) ? f : f.substr(0, b);
        };
        std::string orig = nameOf(img) + nameOf(sub);
        mixinKey_.clear();
        for (int i = 0; i < 64 && i < (int)orig.size(); i++) mixinKey_ += orig[MIXIN_TAB[i]];
        mixinKey_ = mixinKey_.substr(0, 32);
        mixinTs_ = util::nowSec();
    }

    void refreshMixinLocked() {
        auto r = http::get(std::wstring(API_HOST) + L"/x/web-interface/nav", headers());
        if (r.body.empty()) throw BiliError("获取 wbi 签名密钥失败，请检查网络");
        try {
            mixinFromNav(json::parse(r.body));
        } catch (const json::exception&) {
            throw BiliError("wbi 密钥解析失败");
        }
        if (mixinKey_.empty()) throw BiliError("wbi 密钥为空");
    }

    // 保护 cookie / mixinKey_ / mixinTs_。主线程（parse、扫码登录）和每个下载线程
    // 都会碰这几样；并发写 std::string 是 UB，表现就是随机崩。
    // 用递归锁是因为 sign() 持锁期间会经过 refreshMixinLocked() -> headers() 再进一次。
    mutable std::recursive_mutex mtx_;
    std::string cookie;   // "SESSDATA=...; bili_jct=..."，读写一律受 mtx_ 保护
    std::string mixinKey_;
    double mixinTs_ = 0;
};

// Set-Cookie 解析：只保留登录需要的几张票，其余合并保留
inline void Client::collectCookies(const http::Response& r) {
    // 持锁做完整替换：下载线程随时可能在下一次请求里读 cookie，
    // 不能出现"已 clear、还没拼回去"的空窗
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::map<std::string, std::string> kv;
    // 先继承已有 cookie
    {
        size_t pos = 0;
        while (pos < cookie.size()) {
            size_t semi = cookie.find(';', pos);
            std::string item = cookie.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
            pos = (semi == std::string::npos) ? cookie.size() : semi + 1;
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            std::string k = item.substr(0, eq), v = item.substr(eq + 1);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            while (!k.empty() && k.front() == ' ') k.erase(k.begin());
            if (!k.empty() && !v.empty()) kv[k] = v;
        }
    }
    for (auto& line : r.setCookies) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        size_t semi = v.find(';');
        if (semi != std::string::npos) v = v.substr(0, semi);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k.empty() || v.empty() || v == "deleted") continue;
        kv[k] = v;
    }
    cookie.clear();
    for (auto& [k, v] : kv) cookie += k + "=" + v + "; ";
    while (!cookie.empty() && cookie.back() == ' ') cookie.pop_back();
}

}  // namespace bili
