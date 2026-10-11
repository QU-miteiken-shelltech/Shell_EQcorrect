// channels.hpp - マルチチャンネルの「チャンネル名」の扱い
//   * WAV のチャンネル配置 (dwChannelMask またはチャンネル数) → チャンネル名の並び
//   * ファイル名 / "NAME=path" 指定 → チャンネル名 (別名を正規名に変換)
//   * 正規名 → WAV 内のチャンネル番号 (サラウンドの別名吸収を含む)
//
// 正規名 (大文字小文字は区別しない):
//   L R C LFE  Lss Rss (サイド)  Lrs Rrs (リア)  Lc Rc  Cs (リアセンター)
//   Tfl Tfc Tfr Tc Tbl Tbc Tbr (ハイト)
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace eq {
namespace chn {

inline std::string norm_token(const std::string& s) {   // 英数字のみ小文字化 (区切り記号を捨てる)
    std::string o;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') o += (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += (char)c;
    }
    return o;
}

struct Alias { const char* canon; const char* names; };   // names はカンマ区切り (正規化済み)
// 注意: "ls"/"rs" は 5.1 では「サラウンド」、7.1 では「サイド」として使われる慣習。
//       ここでは Lss/Rss (サイド) に割り当て、WAV 側にサイドが無くリアだけがある場合は resolve() がリアへ読み替える。
static const Alias kAliases[] = {
    {"L",   "l,fl,left,frontleft"},
    {"R",   "r,fr,right,frontright"},
    {"C",   "c,fc,center,centre,centerfront"},
    {"LFE", "lfe,sub,subwoofer,sw,lfe1"},
    {"Lss", "lss,ls,sl,sideleft,leftsurround,lsur,lside"},
    {"Rss", "rss,rs,sr,sideright,rightsurround,rsur,rside"},
    {"Lrs", "lrs,bl,rl,lb,lsb,backleft,rearleft,leftrear,leftback,lrear"},
    {"Rrs", "rrs,br,rr,rb,rsb,backright,rearright,rightrear,rightback,rrear"},
    {"Lc",  "lc,flc,frontleftcenter"},
    {"Rc",  "rc,frc,frontrightcenter"},
    {"Cs",  "cs,bc,rearcenter,backcenter"},
    {"Tfl", "tfl,ltf,topfrontleft,lh,lhf"},
    {"Tfc", "tfc,ctf,topfrontcenter"},
    {"Tfr", "tfr,rtf,topfrontright,rh,rhf"},
    {"Tc",  "tc,topcenter"},
    {"Tbl", "tbl,ltb,topbackleft,toprearleft,lhr"},
    {"Tbc", "tbc,ctb,topbackcenter"},
    {"Tbr", "tbr,rtb,topbackright,toprearright,rhr"},
};

// 文字列 (トークン) → 正規名。該当なしは空文字
inline std::string canon_of(const std::string& tok) {
    std::string t = norm_token(tok);
    if (t.empty()) return "";
    for (const Alias& a : kAliases) {
        std::string list = std::string(",") + a.names + ",";
        if (list.find("," + t + ",") != std::string::npos) return a.canon;
        if (norm_token(a.canon) == t) return a.canon;
    }
    return "";
}

// dwChannelMask のビット順 (SPEAKER_*) に対応する正規名
static const char* const kMaskNames[18] = {
    "L", "R", "C", "LFE", "Lrs", "Rrs", "Lc", "Rc", "Cs", "Lss", "Rss",
    "Tc", "Tfl", "Tfc", "Tfr", "Tbl", "Tbc", "Tbr"};

inline int popcnt(uint32_t x) { int n = 0; while (x) { n += (int)(x & 1u); x >>= 1; } return n; }

// チャンネル数のみから標準配置を仮定 (マスクが無いとき)。該当なしなら空
inline std::vector<std::string> default_layout(int n) {
    switch (n) {
        case 1: return {"C"};
        case 2: return {"L", "R"};
        case 3: return {"L", "R", "C"};
        case 4: return {"L", "R", "Lrs", "Rrs"};                          // クワッド 0x33
        case 5: return {"L", "R", "C", "Lrs", "Rrs"};                     // 5.0   0x37
        case 6: return {"L", "R", "C", "LFE", "Lrs", "Rrs"};              // 5.1   0x3F
        case 7: return {"L", "R", "C", "LFE", "Cs", "Lss", "Rss"};        // 6.1   0x70F
        case 8: return {"L", "R", "C", "LFE", "Lrs", "Rrs", "Lss", "Rss"};// 7.1   0x63F
        default: return {};
    }
}

struct Layout {
    std::vector<std::string> names;   // WAV 内のチャンネル順の正規名
    std::string source;               // 由来 (ログ表示用)
};

// 決定順: (1) --channels 指定  (2) dwChannelMask (ビット数 = チャンネル数)  (3) チャンネル数からの標準配置
inline bool make_layout(int nch, uint32_t mask, const std::string& user_list, Layout& out, std::string& err) {
    out = Layout();
    if (!user_list.empty()) {
        std::string cur;
        std::vector<std::string> toks;
        for (size_t i = 0; i <= user_list.size(); i++) {
            if (i == user_list.size() || user_list[i] == ',') { toks.push_back(cur); cur.clear(); }
            else cur += user_list[i];
        }
        if ((int)toks.size() != nch) {
            err = "--channels の個数 (" + std::to_string(toks.size()) + ") が WAV のチャンネル数 (" + std::to_string(nch) + ") と一致しません";
            return false;
        }
        for (auto& t : toks) {
            std::string c = canon_of(t);
            if (c.empty()) { err = "--channels に未知のチャンネル名があります: '" + t + "' (使える名前: L R C LFE Lss Rss Lrs Rrs Lc Rc Cs Tfl Tfc Tfr Tc Tbl Tbc Tbr)"; return false; }
            for (auto& e : out.names) if (e == c) { err = "--channels でチャンネル名が重複しています: " + c; return false; }
            out.names.push_back(c);
        }
        out.source = "--channels 指定";
        return true;
    }
    if (mask != 0 && popcnt(mask) == nch && (mask >> 18) == 0) {
        for (int b = 0; b < 18; b++) if (mask & (1u << b)) out.names.push_back(kMaskNames[b]);
        char b[64]; std::snprintf(b, sizeof b, "WAV のチャンネルマスク 0x%X", (unsigned)mask);
        out.source = b;
        return true;
    }
    out.names = default_layout(nch);
    if (out.names.empty()) {
        err = std::to_string(nch) + " ch の標準配置が分からないため、--channels L,R,C,... でチャンネル名を順番に指定してください";
        return false;
    }
    out.source = (mask != 0 ? "WAV のチャンネルマスクがチャンネル数と合わないため、チャンネル数 " : "WAV にチャンネルマスクが無いため、チャンネル数 ") + std::to_string(nch) + " からの標準配置を仮定";
    return true;
}

// 正規名 → チャンネル番号 (0 始まり)。見つからなければ -1。
// サラウンドの別名吸収: WAV にサイド (Lss/Rss) が無くリア (Lrs/Rrs) だけがある場合、サイド名はリアへ読み替える (逆も同様)。
inline int resolve(const Layout& lo, const std::string& canon, std::string* note = nullptr) {
    auto find = [&](const char* n) { for (size_t i = 0; i < lo.names.size(); i++) if (lo.names[i] == n) return (int)i; return -1; };
    int i = find(canon.c_str());
    if (i >= 0) return i;
    const bool is_side = (canon == "Lss" || canon == "Rss");
    const bool is_rear = (canon == "Lrs" || canon == "Rrs");
    if (!is_side && !is_rear) return -1;
    const bool has_side = find("Lss") >= 0 || find("Rss") >= 0;
    const bool has_rear = find("Lrs") >= 0 || find("Rrs") >= 0;
    // 読み替えるのは、WAV にその系統が全く無く、もう一方の系統だけがある場合に限る
    if ((is_side && !has_side && has_rear) || (is_rear && !has_rear && has_side)) {
        const bool left = (canon[0] == 'L');
        const char* alt = is_side ? (left ? "Lrs" : "Rrs") : (left ? "Lss" : "Rss");
        int j = find(alt);
        if (j >= 0 && note) *note = canon + " は " + alt + " として扱います (この WAV には" + (is_side ? "サイド" : "リア") + "側のサラウンド配置がありません)";
        return j;
    }
    return -1;
}

// ファイル名 (パスは含んでよい) から末尾側のトークンを探してチャンネル名を推定。
// 例: "dir/null_EQ_C.req" → "C",  "room-Lrs.req" → "Lrs"。見つからなければ空
inline std::string canon_from_filename(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    std::string base = (s == std::string::npos) ? path : path.substr(s + 1);
    size_t d = base.find_last_of('.');
    if (d != std::string::npos && d > 0) base = base.substr(0, d);
    std::vector<std::string> toks; std::string cur;
    for (unsigned char c : base) {
        bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (alnum) cur += (char)c;
        else { if (!cur.empty()) toks.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) toks.push_back(cur);
    for (size_t i = toks.size(); i-- > 0;) {
        std::string c = canon_of(toks[i]);
        if (!c.empty()) return c;
    }
    return "";
}

}  // namespace chn
}  // namespace eq
