// WinHTTP 封装：普通请求 + Range 分块下载
#pragma once

#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <functional>

#include "util.h"

namespace http {

using Header = std::pair<std::wstring, std::wstring>;

struct Response {
    int status = 0;
    std::string body;
    std::vector<std::string> setCookies;  // 原始 Set-Cookie 行
    bool ok() const { return status >= 200 && status < 300; }
};

inline std::wstring lastErr() {
    wchar_t buf[128];
    swprintf(buf, 128, L"网络错误 %lu", GetLastError());
    return buf;
}

// 每个线程一个 WinHTTP 会话
struct Net {
    HINTERNET session = nullptr;
    Net() {
        session = WinHttpOpen(L"BiliDown/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session) {
            WinHttpSetTimeouts(session, 12000, 12000, 25000, 45000);
            DWORD prot = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
            WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot));
        }
    }
    ~Net() {
        if (session) WinHttpCloseHandle(session);
    }
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;
};

inline Net& session() {
    thread_local Net net;
    return net;
}

// request + connection 的 RAII 组合，避免连接句柄泄漏
struct Req {
    HINTERNET req = nullptr;
    HINTERNET conn = nullptr;
    Req() = default;
    Req(const Req&) = delete;
    Req& operator=(const Req&) = delete;
    Req(Req&& o) noexcept : req(o.req), conn(o.conn) { o.req = nullptr; o.conn = nullptr; }
    Req& operator=(Req&& o) noexcept {
        if (this != &o) {
            if (req) WinHttpCloseHandle(req);
            if (conn) WinHttpCloseHandle(conn);
            req = o.req; conn = o.conn;
            o.req = nullptr; o.conn = nullptr;
        }
        return *this;
    }
    ~Req() {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
    }
    explicit operator bool() const { return req != nullptr; }
    int status() const {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        return (int)code;
    }
    std::wstring rawHeaders() const {
        DWORD len = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &len, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !len) return L"";
        std::wstring v(len / sizeof(wchar_t) + 2, L'\0');
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &v[0], &len, WINHTTP_NO_HEADER_INDEX))
            return L"";
        v.resize(len / sizeof(wchar_t));
        return v;
    }
};

inline Req open(const std::wstring& url, const std::vector<Header>& headers,
                const std::wstring& method = L"GET", bool followRedirect = true) {
    Req r;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {}, path[8192] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 8192;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return r;

    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    r.conn = WinHttpConnect(session().session, host, uc.nPort ? uc.nPort : (secure ? 443 : 80), 0);
    if (!r.conn) return r;

    r.req = WinHttpOpenRequest(r.conn, method.c_str(), path, nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r.req) return r;

    // 短链解析需要关掉自动跟随，否则拿到的是几百 KB 的视频页，
    // 从里面 find("BV") 第一个命中可能是推荐位而不是目标视频
    DWORD policy = followRedirect ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
                                  : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(r.req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    std::wstring hdr;
    for (auto& h : headers) hdr += h.first + L": " + h.second + L"\r\n";
    if (!hdr.empty())
        WinHttpAddRequestHeaders(r.req, hdr.c_str(), (DWORD)hdr.size(),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(r.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(r.req, nullptr)) {
        WinHttpCloseHandle(r.req);
        r.req = nullptr;
        return r;
    }
    return r;
}

inline Response get(const std::wstring& url, const std::vector<Header>& headers = {}) {
    Response out;
    Req r = open(url, headers);
    if (!r) return out;
    out.status = r.status();

    // 逐行解析响应头里的 Set-Cookie（WinHTTP 只能按索引取单个值，这里直接读原文）
    std::wstring rawW = r.rawHeaders();
    std::string raw = util::narrow(rawW);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find("\r\n", pos);
        std::string line = raw.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? raw.size() : eol + 2;
        if (line.size() > 11) {
            std::string low = line.substr(0, 11);
            for (auto& c : low) c = (char)tolower(c);
            if (low == "set-cookie:") out.setCookies.push_back(line.substr(11));
        }
    }

    DWORD avail = 0;
    std::vector<char> buf(32768);
    while (WinHttpQueryDataAvailable(r.req, &avail) && avail > 0) {
        DWORD got = 0;
        DWORD want = (DWORD)(avail > buf.size() ? buf.size() : avail);
        if (!WinHttpReadData(r.req, buf.data(), want, &got) || got == 0) break;
        out.body.append(buf.data(), got);
    }
    return out;
}

// 探测远程文件总长度；statusOut 带回最后一次响应的状态码（206 表示支持 Range）
inline uint64_t probeSize(const std::wstring& url, const std::vector<Header>& headers,
                          int* statusOut = nullptr) {
    auto h = headers;
    h.push_back({L"Range", L"bytes=0-0"});
    Req r = open(url, h);
    if (!r) return 0;
    if (statusOut) *statusOut = r.status();
    std::wstring raw = r.rawHeaders();
    std::wstring low;
    low.reserve(raw.size());
    for (wchar_t c : raw) low += (wchar_t)towlower(c);

    uint64_t total = 0;
    size_t p = low.find(L"content-range:");
    if (p != std::wstring::npos) {
        size_t slash = raw.find(L'/', p);
        if (slash != std::wstring::npos) total = _wcstoui64(raw.c_str() + slash + 1, nullptr, 10);
    }
    if (!total && r.status() == 200) {
        size_t q = low.find(L"content-length:");
        if (q != std::wstring::npos) total = _wcstoui64(raw.c_str() + q + 15, nullptr, 10);
    }
    return total;
}

// 下载 [offset, offset+len) 区间到文件句柄；返回写入字节数
inline uint64_t downloadRange(const std::wstring& url, const std::vector<Header>& headers,
                              HANDLE file, uint64_t offset, uint64_t len,
                              const std::function<void(uint64_t)>& onChunk,
                              const std::atomic<bool>* cancel) {
    auto h = headers;
    wchar_t range[96];
    swprintf(range, 96, L"bytes=%llu-%llu", offset, offset + len - 1);
    h.push_back({L"Range", range});

    Req r = open(url, h);
    if (!r) return 0;

    // 必须确认服务器真的按 Range 返回了 206。若它忽略 Range 回 200，
    // 响应体是从文件开头算起的整段数据，写进 offset 处会静默错位
    // （结果就是大小完全正确、内容全错的视频，靠文件大小校验查不出来）
    if (r.status() != 206) return 0;

    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(file, pos, nullptr, FILE_BEGIN);

    uint64_t written = 0;
    DWORD avail = 0;
    std::vector<char> buf(262144);
    while (written < len && WinHttpQueryDataAvailable(r.req, &avail) && avail > 0) {
        if (cancel && *cancel) break;
        DWORD want = (DWORD)(avail > buf.size() ? buf.size() : avail);
        if (want > len - written) want = (DWORD)(len - written);
        DWORD got = 0;
        if (!WinHttpReadData(r.req, buf.data(), want, &got) || got == 0) break;
        DWORD w = 0;
        if (!WriteFile(file, buf.data(), got, &w, nullptr) || w != got) break;
        written += w;
        if (onChunk) onChunk(w);
    }
    return written;
}

}  // namespace http
