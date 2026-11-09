// 基础工具：字符串编码、路径、时间
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cctype>

namespace util {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

inline std::string exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return narrow(pos == std::wstring::npos ? L"." : p.substr(0, pos));
}

inline bool fileExists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool dirExists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 往目录里写一个探针文件试试，比查属性可靠（目录存在不代表可写）
inline bool canWrite(const std::string& dirPath) {
    std::wstring probe = widen(dirPath) + L"\\.write_probe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

inline void makeDirs(const std::string& path) {
    std::wstring w = widen(path);
    for (size_t i = 3; i <= w.size(); i++) {
        if (i == w.size() || w[i] == L'\\' || w[i] == L'/') {
            std::wstring sub = w.substr(0, i);
            if (!sub.empty() && sub.back() != L':') CreateDirectoryW(sub.c_str(), nullptr);
        }
    }
}

inline double nowSec() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

// 去掉 Windows 文件名非法字符
inline std::string safeName(const std::string& s, size_t limit = 100) {
    std::string out;
    for (char c : s) {
        unsigned char uc = (unsigned char)c;
        if (uc < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            out += ' ';
        else
            out += c;
    }
    // 压缩连续空格
    std::string r;
    bool sp = false;
    for (char c : out) {
        if (c == ' ') {
            if (!sp) r += c;
            sp = true;
        } else {
            r += c;
            sp = false;
        }
    }
    while (!r.empty() && (r.back() == ' ' || r.back() == '.')) r.pop_back();
    // CON / PRN / NUL / COM1 / LPT1 这类设备名不能当文件名，加前缀避开
    std::string up = r.substr(0, r.find('.'));
    for (char& c : up) c = (char)toupper((unsigned char)c);
    static const char* reserved[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
        "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (auto* n : reserved)
        if (up == n) { r = "_" + r; break; }
    if (r.size() > limit) {
        size_t cut = limit;
        while (cut > 0 && ((unsigned char)r[cut] & 0xC0) == 0x80) cut--;
        r = r.substr(0, cut);
    }
    return r.empty() ? "video" : r;
}

inline std::string fmtSize(double b) {
    const char* u[] = {"B", "KB", "MB", "GB"};
    int i = 0;
    while (b >= 1024 && i < 3) { b /= 1024; i++; }
    char buf[64];
    snprintf(buf, sizeof(buf), (b < 10 && i >= 2) ? "%.1f %s" : "%.0f %s", b, u[i]);
    return buf;
}

}  // namespace util
