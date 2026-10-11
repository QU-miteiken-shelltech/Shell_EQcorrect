// javaser.hpp - Java Object Serialization Stream (java.io.ObjectOutputStream) の汎用パーサー
//
// 目的: クラス定義(.class)が手元になくても、ストリームに含まれる
//       「数値配列 / 数値コレクション / スカラー / 文字列」を取り出す。
// 仕様: Java Object Serialization Specification, Chapter 6 (Object Serialization Stream Protocol)
//
// 対応: TC_OBJECT / TC_ARRAY / TC_STRING / TC_LONGSTRING / TC_CLASS / TC_ENUM / TC_REFERENCE /
//       TC_CLASSDESC / TC_PROXYCLASSDESC / TC_BLOCKDATA(LONG) / TC_RESET、writeObject() 独自データ(annotation)
// 非対応: Externalizable の protocol version 1 形式 (SC_BLOCK_DATA なし)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace eq {
namespace jser {

struct Node;
using NodeP = std::shared_ptr<Node>;

struct FieldDesc { char type = 0; std::string name; std::string cls; };
struct ClassDesc {
    std::string name;
    uint8_t flags = 0;
    std::vector<FieldDesc> fields;
    std::shared_ptr<ClassDesc> super;
};
using ClassDescP = std::shared_ptr<ClassDesc>;

struct Node {
    enum Kind { Obj, Arr, Str, Cls, Enum } kind = Obj;
    std::string cls;                                   // クラス名 (Arr は "[D" 等)
    std::string str;                                   // Str の値 / Enum 定数名
    std::vector<std::pair<std::string, double>> prims; // Obj: プリミティブフィールド
    std::vector<std::pair<std::string, NodeP>> refs;   // Obj: 参照フィールド (null は nullptr)
    std::vector<NodeP> annot;                          // Obj: writeObject() が書いたオブジェクト列 (ArrayList の要素など)
    size_t block_bytes = 0;                            // Obj: 独自ブロックデータのバイト数 (内容は使わない)
    char elem_type = 0;                                // Arr: 要素型コード ('D','F','I','J','S','B','C','Z','L','[')
    std::vector<double> nums;                          // Arr(プリミティブ): 値
    std::vector<NodeP> elems;                          // Arr(オブジェクト): 要素
};

class Parser {
public:
    NodeP root;                        // 最初のトップレベルオブジェクト
    std::vector<NodeP> top_objects;    // すべてのトップレベルオブジェクト

    bool parse(const unsigned char* data, size_t n, std::string& err) {
        p_ = data; n_ = n; pos_ = 0; handles_.clear(); depth_ = 0;
        try {
            if (n < 4 || u16() != 0xACED) throw std::runtime_error("Java シリアライズのマジックナンバー(AC ED)がありません");
            int ver = u16();
            if (ver != 5) throw std::runtime_error("未対応のストリームバージョン: " + std::to_string(ver));
            while (pos_ < n_) {
                NodeP o = read_content(nullptr);
                if (o) { top_objects.push_back(o); if (!root) root = o; }
            }
        } catch (const std::exception& e) {
            std::string msg = std::string("Javaシリアライズ解析エラー (offset ") + std::to_string(pos_) + "): " + e.what();
            if (root) { warning = msg + " ※途中までの結果を使用します"; return true; }  // 途中まで読めていれば部分結果を使う
            err = msg;
            return false;
        }
        return true;
    }

    std::string warning;  // 部分結果で続行した場合の注意

private:
    struct H { NodeP node; ClassDescP cd; };
    const unsigned char* p_ = nullptr;
    size_t n_ = 0, pos_ = 0;
    std::vector<H> handles_;
    int depth_ = 0;

    static constexpr uint8_t TC_NULL = 0x70, TC_REFERENCE = 0x71, TC_CLASSDESC = 0x72, TC_OBJECT = 0x73,
                             TC_STRING = 0x74, TC_ARRAY = 0x75, TC_CLASS = 0x76, TC_BLOCKDATA = 0x77,
                             TC_ENDBLOCKDATA = 0x78, TC_RESET = 0x79, TC_BLOCKDATALONG = 0x7A,
                             TC_EXCEPTION = 0x7B, TC_LONGSTRING = 0x7C, TC_PROXYCLASSDESC = 0x7D, TC_ENUM = 0x7E;
    static constexpr uint32_t kBaseHandle = 0x7E0000;

    void need(size_t k) { if (k > n_ - pos_) throw std::runtime_error("ストリームが途中で終わっています"); }
    uint8_t u8() { need(1); return p_[pos_++]; }
    uint16_t u16() { need(2); uint16_t v = (uint16_t)((p_[pos_] << 8) | p_[pos_ + 1]); pos_ += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = ((uint32_t)p_[pos_] << 24) | ((uint32_t)p_[pos_ + 1] << 16) | ((uint32_t)p_[pos_ + 2] << 8) | p_[pos_ + 3]; pos_ += 4; return v; }
    uint64_t u64() { uint64_t hi = u32(); uint64_t lo = u32(); return (hi << 32) | lo; }
    double f64() { uint64_t b = u64(); double d; std::memcpy(&d, &b, 8); return d; }
    double f32() { uint32_t b = u32(); float f; std::memcpy(&f, &b, 4); return f; }
    // 表示・ログ用の文字列。不正な UTF-8 や制御文字は '?' に置換する (壊れたファイルを端末へそのまま出さない)
    static std::string sanitize(const unsigned char* q, size_t len) {
        std::string out;
        for (size_t i = 0; i < len;) {
            unsigned char c = q[i];
            int n = c < 0x80 ? 0 : (c >= 0xC2 && c < 0xE0) ? 1 : (c >= 0xE0 && c < 0xF0) ? 2 : (c >= 0xF0 && c < 0xF5) ? 3 : -1;
            if (n == 0) { out += (c < 0x20 || c == 0x7F) ? '?' : (char)c; i++; continue; }
            bool ok = n > 0 && i + (size_t)n < len;   // 継続バイトが n 個ぶん残っているか
            if (ok) for (int k = 1; k <= n; k++) if ((q[i + k] & 0xC0) != 0x80) { ok = false; break; }
            if (ok) { out.append((const char*)q + i, (size_t)n + 1); i += (size_t)n + 1; }
            else { out += '?'; i++; }
        }
        return out;
    }
    std::string utf(size_t len) { need(len); std::string s = sanitize(p_ + pos_, len); pos_ += len; return s; }
    std::string utf16() { return utf(u16()); }

    size_t new_handle() { handles_.push_back(H()); return handles_.size() - 1; }

    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& pp) : p(pp) { if (++p.depth_ > 800) throw std::runtime_error("ネストが深すぎます"); }
        ~DepthGuard() { --p.depth_; }
    };

    // コンテンツ1要素 (オブジェクト or ブロックデータ)。blk_owner が与えられればブロックデータ長を加算
    NodeP read_content(Node* blk_owner) {
        uint8_t tc = u8();
        if (tc == TC_BLOCKDATA) { size_t len = u8(); need(len); pos_ += len; if (blk_owner) blk_owner->block_bytes += len; return nullptr; }
        if (tc == TC_BLOCKDATALONG) { size_t len = u32(); need(len); pos_ += len; if (blk_owner) blk_owner->block_bytes += len; return nullptr; }
        return read_object(tc);
    }

    NodeP read_object(uint8_t tc) {
        DepthGuard g(*this);
        switch (tc) {
        case TC_NULL: return nullptr;
        case TC_REFERENCE: {
            uint32_t h = u32() - kBaseHandle;
            if (h >= handles_.size()) throw std::runtime_error("不正な参照ハンドル");
            return handles_[h].node;
        }
        case TC_OBJECT: {
            ClassDescP cd = read_class_desc();
            if (!cd) throw std::runtime_error("TC_OBJECT のクラス記述が null");
            auto node = std::make_shared<Node>();
            node->kind = Node::Obj; node->cls = cd->name;
            size_t h = new_handle(); handles_[h].node = node;
            read_class_data(*node, cd);
            return node;
        }
        case TC_ARRAY: {
            ClassDescP cd = read_class_desc();
            if (!cd || cd->name.size() < 2 || cd->name[0] != '[') throw std::runtime_error("配列のクラス記述が不正");
            auto node = std::make_shared<Node>();
            node->kind = Node::Arr; node->cls = cd->name; node->elem_type = cd->name[1];
            size_t h = new_handle(); handles_[h].node = node;
            uint32_t cnt = u32();
            if (cnt > 0x7FFFFFFFu) throw std::runtime_error("配列サイズが不正");
            char t = node->elem_type;
            size_t esz = (t == 'D' || t == 'J') ? 8 : (t == 'F' || t == 'I') ? 4 : (t == 'S' || t == 'C') ? 2 : (t == 'B' || t == 'Z') ? 1 : 0;
            if (esz) {
                if ((uint64_t)cnt * esz > n_ - pos_) throw std::runtime_error("配列がストリーム長を超えています");
                node->nums.resize(cnt);
                for (uint32_t i = 0; i < cnt; i++) {
                    double v = 0;
                    switch (t) {
                    case 'D': v = f64(); break;
                    case 'F': v = f32(); break;
                    case 'J': v = (double)(int64_t)u64(); break;
                    case 'I': v = (double)(int32_t)u32(); break;
                    case 'S': v = (double)(int16_t)u16(); break;
                    case 'C': v = (double)u16(); break;
                    case 'B': v = (double)(int8_t)u8(); break;
                    case 'Z': v = u8() ? 1.0 : 0.0; break;
                    }
                    node->nums[i] = v;
                }
            } else {
                if (cnt > n_ - pos_) throw std::runtime_error("配列サイズがストリーム長を超えています");
                node->elems.reserve(cnt);
                for (uint32_t i = 0; i < cnt; i++) node->elems.push_back(read_content(nullptr));
            }
            return node;
        }
        case TC_STRING: case TC_LONGSTRING: {
            auto node = std::make_shared<Node>();
            node->kind = Node::Str;
            size_t h = new_handle(); handles_[h].node = node;
            if (tc == TC_STRING) node->str = utf16();
            else { uint64_t len = u64(); if (len > n_ - pos_) throw std::runtime_error("文字列長が不正"); node->str = utf((size_t)len); }
            return node;
        }
        case TC_CLASS: {
            ClassDescP cd = read_class_desc();
            auto node = std::make_shared<Node>();
            node->kind = Node::Cls; node->cls = cd ? cd->name : "";
            size_t h = new_handle(); handles_[h].node = node;
            return node;
        }
        case TC_ENUM: {
            ClassDescP cd = read_class_desc();
            auto node = std::make_shared<Node>();
            node->kind = Node::Enum; node->cls = cd ? cd->name : "";
            size_t h = new_handle(); handles_[h].node = node;
            NodeP nm = read_content(nullptr);
            if (nm) node->str = nm->str;
            return node;
        }
        case TC_CLASSDESC: case TC_PROXYCLASSDESC:
            pos_--; read_class_desc(); return nullptr;
        case TC_RESET:
            handles_.clear(); return nullptr;
        case TC_EXCEPTION:
            throw std::runtime_error("ストリームに例外オブジェクトが含まれています");
        default:
            throw std::runtime_error("未知のタイプコード 0x" + hex(tc));
        }
    }

    static std::string hex(unsigned v) { char b[16]; std::snprintf(b, sizeof b, "%02X", v); return b; }

    ClassDescP read_class_desc() {
        DepthGuard g(*this);
        uint8_t tc = u8();
        if (tc == TC_NULL) return nullptr;
        if (tc == TC_REFERENCE) {
            uint32_t h = u32() - kBaseHandle;
            if (h >= handles_.size() || !handles_[h].cd) throw std::runtime_error("クラス記述の参照が不正");
            return handles_[h].cd;
        }
        if (tc == TC_CLASSDESC) {
            auto cd = std::make_shared<ClassDesc>();
            cd->name = utf16();
            need(8); pos_ += 8;  // serialVersionUID
            size_t h = new_handle(); handles_[h].cd = cd;
            cd->flags = u8();
            uint16_t nf = u16();
            for (uint16_t i = 0; i < nf; i++) {
                FieldDesc fd;
                fd.type = (char)u8();
                fd.name = utf16();
                if (fd.type == 'L' || fd.type == '[') {
                    NodeP cn = read_content(nullptr);  // クラス名文字列 (TC_STRING / TC_REFERENCE)
                    if (cn) fd.cls = cn->str;
                }
                cd->fields.push_back(fd);
            }
            skip_annotation();
            cd->super = read_class_desc();
            return cd;
        }
        if (tc == TC_PROXYCLASSDESC) {
            auto cd = std::make_shared<ClassDesc>();
            cd->name = "<proxy>";
            size_t h = new_handle(); handles_[h].cd = cd;
            uint32_t cnt = u32();
            for (uint32_t i = 0; i < cnt; i++) utf16();
            skip_annotation();
            cd->super = read_class_desc();
            return cd;
        }
        throw std::runtime_error("クラス記述が期待される位置に 0x" + hex(tc));
    }

    void skip_annotation() {
        for (;;) {
            uint8_t tc = u8();
            if (tc == TC_ENDBLOCKDATA) return;
            pos_--;
            read_content(nullptr);
        }
    }

    void read_annotation(Node& node) {
        for (;;) {
            uint8_t tc = u8();
            if (tc == TC_ENDBLOCKDATA) return;
            pos_--;
            NodeP o = read_content(&node);
            if (o) node.annot.push_back(o);
        }
    }

    void read_class_data(Node& node, const ClassDescP& leaf) {
        std::vector<ClassDescP> chain;
        for (ClassDescP c = leaf; c; c = c->super) chain.push_back(c);
        std::reverse(chain.begin(), chain.end());  // スーパークラス → サブクラスの順
        for (auto& cd : chain) {
            constexpr uint8_t SC_WRITE_METHOD = 0x01, SC_SERIALIZABLE = 0x02, SC_EXTERNALIZABLE = 0x04, SC_BLOCK_DATA = 0x08;
            if (cd->flags & SC_EXTERNALIZABLE) {
                if (!(cd->flags & SC_BLOCK_DATA)) throw std::runtime_error("protocol version 1 の Externalizable には非対応");
                read_annotation(node);
                continue;
            }
            if (cd->flags & SC_SERIALIZABLE) {
                for (auto& fd : cd->fields) {
                    switch (fd.type) {
                    case 'B': node.prims.emplace_back(fd.name, (double)(int8_t)u8()); break;
                    case 'C': node.prims.emplace_back(fd.name, (double)u16()); break;
                    case 'D': node.prims.emplace_back(fd.name, f64()); break;
                    case 'F': node.prims.emplace_back(fd.name, f32()); break;
                    case 'I': node.prims.emplace_back(fd.name, (double)(int32_t)u32()); break;
                    case 'J': node.prims.emplace_back(fd.name, (double)(int64_t)u64()); break;
                    case 'S': node.prims.emplace_back(fd.name, (double)(int16_t)u16()); break;
                    case 'Z': node.prims.emplace_back(fd.name, u8() ? 1.0 : 0.0); break;
                    case 'L': case '[': node.refs.emplace_back(fd.name, read_content(nullptr)); break;
                    default: throw std::runtime_error("未知のフィールド型コード 0x" + hex((unsigned char)fd.type));
                    }
                }
                if (cd->flags & SC_WRITE_METHOD) read_annotation(node);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// 抽出: ストリームから数値列・スカラー・文字列を「パス付き」で集める
// ---------------------------------------------------------------------------
struct NumList { std::string path; std::vector<double> v; };
struct Scalar { std::string path; double v; };
struct StrItem { std::string path; std::string v; };

struct Extract {
    std::vector<NumList> lists;
    std::vector<Scalar> scalars;
    std::vector<StrItem> strings;
};

inline std::string simple_name(const std::string& cls) {
    size_t i = cls.find_last_of(".$");
    return i == std::string::npos ? cls : cls.substr(i + 1);
}

inline bool is_num_wrapper(const NodeP& n, double& out) {
    if (!n || n->kind != Node::Obj) return false;
    static const char* names[] = {"java.lang.Double", "java.lang.Float", "java.lang.Integer", "java.lang.Long",
                                  "java.lang.Short", "java.lang.Byte"};
    bool ok = false;
    for (auto* nm : names) if (n->cls == nm) ok = true;
    if (!ok) return false;
    for (auto& kv : n->prims) if (kv.first == "value") { out = kv.second; return true; }
    return false;
}

class Collector {
public:
    Extract ex;
    void run(const std::vector<NodeP>& tops) {
        int i = 0;
        for (auto& t : tops) {
            if (!t) continue;
            std::string base = (t->kind == Node::Obj) ? simple_name(t->cls) : (t->kind == Node::Arr ? "array" : "value");
            if (tops.size() > 1) base += "#" + std::to_string(i);
            visit(t, base);
            i++;
        }
    }

private:
    std::set<const Node*> seen_;

    void visit(const NodeP& n, const std::string& path) {
        if (!n) return;
        if (n->kind == Node::Str) { ex.strings.push_back({path, n->str}); return; }
        if (n->kind == Node::Enum) { ex.strings.push_back({path, simple_name(n->cls) + "." + n->str}); return; }
        if (n->kind == Node::Cls) return;
        if (!seen_.insert(n.get()).second) return;  // 循環/共有参照は一度だけ

        if (n->kind == Node::Arr) {
            const bool is_prim = (n->elem_type != 'L' && n->elem_type != '[');
            if (is_prim) { if (!n->nums.empty()) ex.lists.push_back({path, n->nums}); }
            else children(n->elems, path);
            return;
        }
        // Obj
        for (auto& kv : n->prims) ex.scalars.push_back({path + "." + kv.first, kv.second});
        for (auto& kv : n->refs) visit(kv.second, path + "." + kv.first);
        if (!n->annot.empty()) {
            // HashMap / Hashtable / TreeMap 等: writeObject が「キー, 値, キー, 値, ...」を書く。
            // キー文字列を名前として使う (例: HashMap.freq, HashMap.gain)
            const std::string sn = simple_name(n->cls);
            bool map_like = (sn.size() >= 3 && sn.compare(sn.size() - 3, 3, "Map") == 0) || sn == "Hashtable" || sn == "Properties";
            if (map_like && n->annot.size() % 2 == 0 && n->annot[0] && n->annot[0]->kind == Node::Str) {
                for (size_t i = 0; i + 1 < n->annot.size(); i += 2)
                    if (n->annot[i] && n->annot[i]->kind == Node::Str) visit(n->annot[i + 1], path + "." + n->annot[i]->str);
            } else {
                children(n->annot, path + ".<items>");
            }
        }
    }

    // 子要素の集合 (配列要素 / コレクション要素) を処理
    void children(const std::vector<NodeP>& ch, const std::string& path) {
        // (1) 数値ラッパー (Double など) の列 → 1本の数値列
        {
            std::vector<double> v; bool all = !ch.empty();
            for (auto& c : ch) { double x; if (is_num_wrapper(c, x)) v.push_back(x); else { all = false; break; } }
            if (all && v.size() >= 2) { ex.lists.push_back({path, v}); return; }
        }
        // (2) 同一クラスのオブジェクト列 → フィールドごとの列 (例: List<Point{freq,gain}>)
        {
            bool same = ch.size() >= 2 && ch[0] && ch[0]->kind == Node::Obj;
            for (auto& c : ch) if (!c || c->kind != Node::Obj || c->cls != ch[0]->cls) { same = false; break; }
            if (same) {
                for (size_t k = 0; k < ch[0]->prims.size(); k++) {
                    std::vector<double> col;
                    bool ok = true;
                    for (auto& c : ch) {
                        if (k >= c->prims.size() || c->prims[k].first != ch[0]->prims[k].first) { ok = false; break; }
                        col.push_back(c->prims[k].second);
                    }
                    if (ok) ex.lists.push_back({path + "[]." + ch[0]->prims[k].first, col});
                }
                // 参照フィールド (配列など) は先頭要素のみ辿る
                visit_first_refs(ch[0], path + "[0]");
                return;
            }
        }
        // (3) 同じ長さの小さな数値配列が多数 → 2次元配列 (行 = [f, dB, phase] など)。列ごとの列も追加
        {
            bool rows = ch.size() >= 8;
            size_t w = 0;
            for (auto& c : ch) {
                if (!c || c->kind != Node::Arr || c->nums.empty() || c->nums.size() > 8) { rows = false; break; }
                if (w == 0) w = c->nums.size(); else if (c->nums.size() != w) { rows = false; break; }
            }
            if (rows) {
                for (size_t j = 0; j < w; j++) {
                    std::vector<double> col;
                    for (auto& c : ch) col.push_back(c->nums[j]);
                    ex.lists.push_back({path + "[:," + std::to_string(j) + "]", col});
                }
                return;
            }
        }
        // (4) それ以外は個別に辿る
        for (size_t i = 0; i < ch.size(); i++) visit(ch[i], path + "[" + std::to_string(i) + "]");
    }

    void visit_first_refs(const NodeP& n, const std::string& path) {
        for (auto& kv : n->refs) visit(kv.second, path + "." + kv.first);
    }
};

// ---------------------------------------------------------------------------
// 構造ダンプ (人間が読む用)。.req の中身が分からない時の調査に使う
// ---------------------------------------------------------------------------
inline size_t& dump_limit() { static size_t v = 3; return v; }   // ダンプで表示する配列要素数 (--dump-full で拡大)

inline void dump_node(const NodeP& n, int indent, int depth, std::set<const Node*>& seen, std::string& out) {
    std::string pad(indent * 2, ' ');
    if (!n) { out += pad + "null\n"; return; }
    char buf[256];
    auto summarize = [&](const std::vector<double>& v) {
        double mn = INFINITY, mx = -INFINITY; bool mono = true;
        for (size_t i = 0; i < v.size(); i++) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); if (i && v[i] < v[i - 1]) mono = false; }
        std::string s = "len=" + std::to_string(v.size());
        std::snprintf(buf, sizeof buf, " min=%.6g max=%.6g%s first=[", mn, mx, mono ? " (単調増加)" : "");
        s += buf;
        for (size_t i = 0; i < v.size() && i < 4; i++) { std::snprintf(buf, sizeof buf, "%s%.6g", i ? ", " : "", v[i]); s += buf; }
        s += v.size() > 4 ? ", ...]" : "]";
        return s;
    };
    switch (n->kind) {
    case Node::Str: out += pad + "\"" + n->str.substr(0, 120) + "\"\n"; return;
    case Node::Enum: out += pad + "enum " + n->cls + "." + n->str + "\n"; return;
    case Node::Cls: out += pad + "class " + n->cls + "\n"; return;
    case Node::Arr:
        if (!n->nums.empty() || (n->elem_type != 'L' && n->elem_type != '[')) {
            out += pad + n->cls + " " + (n->nums.empty() ? std::string("len=0") : summarize(n->nums)) + "\n";
        } else {
            out += pad + n->cls + " (" + std::to_string(n->elems.size()) + " 要素)\n";
            for (size_t i = 0; i < n->elems.size() && i < dump_limit(); i++) dump_node(n->elems[i], indent + 1, depth + 1, seen, out);
            if (n->elems.size() > dump_limit()) out += pad + "  ... 残り " + std::to_string(n->elems.size() - dump_limit()) + " 要素\n";
        }
        return;
    case Node::Obj: break;
    }
    if (!seen.insert(n.get()).second) { out += pad + n->cls + " (既出の参照)\n"; return; }
    out += pad + n->cls + " {\n";
    for (auto& kv : n->prims) { std::snprintf(buf, sizeof buf, "%.17g", kv.second); out += pad + "  " + kv.first + " = " + buf + "\n"; }
    for (auto& kv : n->refs) { out += pad + "  " + kv.first + ":\n"; dump_node(kv.second, indent + 2, depth + 1, seen, out); }
    if (!n->annot.empty() || n->block_bytes) {
        out += pad + "  <writeObject データ: ブロック " + std::to_string(n->block_bytes) + " byte, オブジェクト " + std::to_string(n->annot.size()) + " 個>\n";
        for (size_t i = 0; i < n->annot.size() && i < 3; i++) dump_node(n->annot[i], indent + 2, depth + 1, seen, out);
        if (n->annot.size() > 3) out += pad + "    ... 残り " + std::to_string(n->annot.size() - 3) + " 個\n";
    }
    out += pad + "}\n";
}

}  // namespace jser
}  // namespace eq
