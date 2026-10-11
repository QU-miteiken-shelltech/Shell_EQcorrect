// platform.hpp - Windows/Linux 差を吸収する薄い層
// ・ファイルパスは内部では常に UTF-8 として扱う
// ・Windows では _wfopen / CommandLineToArgvW 経由にして日本語ファイル名を扱えるようにする
#pragma once
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace eq {

#ifdef _WIN32
inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string wide_to_utf8(const wchar_t* w) {
    int len = (int)wcslen(w);
    if (len == 0) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    return s;
}
inline FILE* fopen_u8(const std::string& path, const char* mode) {
    std::wstring wm = utf8_to_wide(mode);
    return _wfopen(utf8_to_wide(path).c_str(), wm.c_str());
}
// main() の argv は ANSI コードページなので、UTF-16 の生コマンドラインから取り直す
inline std::vector<std::string> get_args_utf8(int, char**) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    for (int i = 0; i < argc; i++) out.push_back(wide_to_utf8(wargv[i]));
    LocalFree(wargv);
    return out;
}
inline void console_utf8() { SetConsoleOutputCP(CP_UTF8); }
#else
inline FILE* fopen_u8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
inline std::vector<std::string> get_args_utf8(int argc, char** argv) {
    return std::vector<std::string>(argv, argv + argc);
}
inline void console_utf8() {}
#endif

// ファイル全体を読み込む
inline bool read_file(const std::string& path, std::vector<unsigned char>& out, std::string& err) {
    FILE* f = fopen_u8(path, "rb");
    if (!f) { err = "ファイルを開けません: " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    long long sz = 0;
#ifdef _WIN32
    sz = _ftelli64(f);
#else
    sz = ftell(f);
#endif
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); err = "ファイルサイズを取得できません: " + path; return false; }
    out.resize((size_t)sz);
    size_t rd = sz ? std::fread(out.data(), 1, (size_t)sz, f) : 0;
    std::fclose(f);
    if (rd != (size_t)sz) { err = "読み込みエラー: " + path; return false; }
    return true;
}

struct Timer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double sec() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
};

inline std::string basename_of(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

}  // namespace eq
