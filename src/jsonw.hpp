// jsonw.hpp - 依存なしの最小JSONライター (NaN/Inf は null、不正UTF-8は '?' に置換)
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace eq {

class JsonWriter {
public:
    std::string s;

    void begin_obj() { sep(); s += '{'; first_.push_back(true); }
    void end_obj() { s += '}'; first_.pop_back(); }
    void begin_arr() { sep(); s += '['; first_.push_back(true); }
    void end_arr() { s += ']'; first_.pop_back(); }

    void key(const char* k) { sep(); str_raw(k); s += ':'; pending_value_ = true; }

    void val(double v) {
        sep();
        if (!std::isfinite(v)) { s += "null"; return; }
        char b[40];
        std::snprintf(b, sizeof b, "%.7g", v);
        s += b;
    }
    void val(long long v) { sep(); s += std::to_string(v); }
    void val(int v) { val((long long)v); }
    void val(size_t v) { val((long long)v); }
    void val(bool v) { sep(); s += v ? "true" : "false"; }
    void val(const std::string& v) { sep(); str_raw(v); }
    void val(const char* v) { sep(); str_raw(v); }
    void null() { sep(); s += "null"; }

    void arr(const char* k, const std::vector<double>& v) {
        key(k); begin_arr(); for (double x : v) val(x); end_arr();
    }
    template <class T> void kv(const char* k, const T& v) { key(k); val(v); }

private:
    std::vector<bool> first_;
    bool pending_value_ = false;

    void sep() {
        if (pending_value_) { pending_value_ = false; return; }
        if (!first_.empty()) { if (!first_.back()) s += ','; first_.back() = false; }
    }
    void str_raw(const std::string& v) {
        s += '"';
        for (size_t i = 0; i < v.size(); i++) {
            unsigned char c = (unsigned char)v[i];
            if (c == '"') s += "\\\"";
            else if (c == '\\') s += "\\\\";
            else if (c == '\n') s += "\\n";
            else if (c == '\r') s += "\\r";
            else if (c == '\t') s += "\\t";
            else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); s += b; }
            else if (c < 0x80) s += (char)c;
            else {
                int n = (c >= 0xF0 && c < 0xF8) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC2 && c < 0xE0) ? 1 : -1;
                bool ok = n > 0 && i + (size_t)n < v.size();
                if (ok) for (int k = 1; k <= n; k++) if (((unsigned char)v[i + k] & 0xC0) != 0x80) ok = false;
                if (ok) { s.append(v, i, n + 1); i += n; }
                else s += '?';
            }
        }
        s += '"';
    }
};

}  // namespace eq
