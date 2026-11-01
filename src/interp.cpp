#include "interp.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <ostream>
#include <sstream>
#include <thread>
#include <set>
#include <unordered_map>

namespace tide {

using namespace ast;
using sema::IntWidth;
using sema::Symbol;
using sema::SymbolKind;
using sema::TypeKind;
using sol::Bytes;
using sol::Pubkey;
using Json = nlohmann::json;

namespace {

// ================================================================= values ==

struct Void {};
struct Null {};
struct Duration {
    std::int64_t ms;
};
struct SourceV {
    std::string url;
};
struct RangeV {
    __int128 lo, hi;
    bool inclusive;
};
struct MetaV {
    Pubkey key;
    bool signer, writable;
};
struct SchemaV {
    const Symbol* sym;
};
struct ModuleV {
    const sema::Module* mod;
};

struct Value;
struct RecordData;
using ListP = std::shared_ptr<std::vector<Value>>;
using RecordP = std::shared_ptr<RecordData>;

struct Value {
    std::variant<Void, Null, bool, __int128, double, std::string, Bytes, Duration, Pubkey, sol::Keypair, SourceV, RangeV,
                 ListP, RecordP, MetaV, SchemaV, ModuleV>
        v;

    Value() : v(Void{}) {}
    template <typename T>
        requires(!std::is_same_v<std::decay_t<T>, Value>)
    Value(T&& x) : v(std::forward<T>(x)) {}

    template <typename T>
    bool is() const { return std::holds_alternative<T>(v); }
    template <typename T>
    const T& as() const { return std::get<T>(v); }
    template <typename T>
    T& as() { return std::get<T>(v); }
};

struct RecordData {
    std::vector<std::pair<std::string, Value>> fields;
    Value* find(std::string_view name) {
        for (auto& [n, v] : fields)
            if (n == name) return &v;
        return nullptr;
    }
    const Value* find(std::string_view name) const {
        for (const auto& [n, v] : fields)
            if (n == name) return &v;
        return nullptr;
    }
};

Value make_list(std::vector<Value> items) { return Value(std::make_shared<std::vector<Value>>(std::move(items))); }
Value make_record(std::vector<std::pair<std::string, Value>> fields) {
    auto r = std::make_shared<RecordData>();
    r->fields = std::move(fields);
    return Value(std::move(r));
}

[[noreturn]] void fail(Loc loc, const std::string& msg) { throw TideError(loc, msg); }

// Ends a dispatch over block keys. Sema's field specs decide which keys exist;
// reaching this means a key was added there without an interpreter handler.
[[noreturn]] void unhandled_key(std::string_view block, const std::string& key) {
    throw std::logic_error("internal error: " + std::string(block) + " key '" + key +
                           "' passed the checker but has no interpreter handler");
}

std::string int_to_string(__int128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? -static_cast<unsigned __int128>(v) : static_cast<unsigned __int128>(v);
    std::string s;
    while (u) {
        s += static_cast<char>('0' + static_cast<int>(u % 10));
        u /= 10;
    }
    if (neg) s += '-';
    return {s.rbegin(), s.rend()};
}

std::string format_value(const Value& v, bool quote_strings = false) {
    return std::visit(
        overloaded{
            [](const Void&) -> std::string { return "void"; },
            [](const Null&) -> std::string { return "null"; },
            [](bool b) -> std::string { return b ? "true" : "false"; },
            [](__int128 i) { return int_to_string(i); },
            [](double d) {
                std::ostringstream ss;
                ss.precision(15);
                ss << d;
                return ss.str();
            },
            [&](const std::string& s) { return quote_strings ? "\"" + s + "\"" : s; },
            [](const Bytes& b) { return "0x" + sol::hex_encode(b); },
            [](const Duration& d) { return std::to_string(d.ms) + "ms"; },
            [](const Pubkey& p) { return p.base58(); },
            [](const sol::Keypair& k) { return "keypair(" + k.pubkey.base58() + ")"; },
            [](const SourceV& s) { return "source(" + s.url + ")"; },
            [](const RangeV& r) { return int_to_string(r.lo) + (r.inclusive ? "..=" : "..") + int_to_string(r.hi); },
            [](const ListP& l) {
                std::string s = "[";
                for (std::size_t i = 0; i < l->size(); ++i) s += (i ? ", " : "") + format_value((*l)[i], true);
                return s + "]";
            },
            [](const RecordP& r) {
                std::string s = "{";
                for (std::size_t i = 0; i < r->fields.size(); ++i)
                    s += (i ? ", " : "") + r->fields[i].first + ": " + format_value(r->fields[i].second, true);
                return s + "}";
            },
            [](const MetaV& m) {
                return std::string(m.signer ? "signer" : m.writable ? "writable" : "readonly") + "(" + m.key.base58() + ")";
            },
            [](const SchemaV& s) { return "schema " + s.sym->name; },
            [](const ModuleV&) -> std::string { return "module"; },
        },
        v.v);
}

bool values_equal(const Value& a, const Value& b) {
    if (a.is<__int128>() && b.is<double>()) return static_cast<double>(a.as<__int128>()) == b.as<double>();
    if (a.is<double>() && b.is<__int128>()) return a.as<double>() == static_cast<double>(b.as<__int128>());
    if (a.is<Bytes>() && b.is<ListP>()) return values_equal(b, a);
    if (a.is<ListP>() && b.is<Bytes>()) {
        const auto& l = *a.as<ListP>();
        const auto& bytes = b.as<Bytes>();
        if (l.size() != bytes.size()) return false;
        for (std::size_t i = 0; i < l.size(); ++i)
            if (!l[i].is<__int128>() || l[i].as<__int128>() != bytes[i]) return false;
        return true;
    }
    if (a.v.index() != b.v.index()) return false;
    return std::visit(
        overloaded{
            [](const Void&) { return true; },
            [](const Null&) { return true; },
            [&](bool x) { return x == b.as<bool>(); },
            [&](__int128 x) { return x == b.as<__int128>(); },
            [&](double x) { return x == b.as<double>(); },
            [&](const std::string& x) { return x == b.as<std::string>(); },
            [&](const Bytes& x) { return x == b.as<Bytes>(); },
            [&](const Duration& x) { return x.ms == b.as<Duration>().ms; },
            [&](const Pubkey& x) { return x == b.as<Pubkey>(); },
            [&](const sol::Keypair& x) { return x.pubkey == b.as<sol::Keypair>().pubkey; },
            [&](const SourceV& x) { return x.url == b.as<SourceV>().url; },
            [&](const RangeV& x) {
                const auto& y = b.as<RangeV>();
                return x.lo == y.lo && x.hi == y.hi && x.inclusive == y.inclusive;
            },
            [&](const ListP& x) {
                const auto& y = b.as<ListP>();
                if (x->size() != y->size()) return false;
                for (std::size_t i = 0; i < x->size(); ++i)
                    if (!values_equal((*x)[i], (*y)[i])) return false;
                return true;
            },
            [&](const RecordP& x) {
                const auto& y = b.as<RecordP>();
                if (x->fields.size() != y->fields.size()) return false;
                for (const auto& [n, v] : x->fields) {
                    const Value* o = y->find(n);
                    if (!o || !values_equal(v, *o)) return false;
                }
                return true;
            },
            [&](const MetaV& x) {
                const auto& y = b.as<MetaV>();
                return x.key == y.key && x.signer == y.signer && x.writable == y.writable;
            },
            [&](const SchemaV& x) { return x.sym == b.as<SchemaV>().sym; },
            [&](const ModuleV& x) { return x.mod == b.as<ModuleV>().mod; },
        },
        a.v);
}

// =============================================================== encoding ==

// Binary layout descriptor shared by Tide schemas and Anchor IDL types.
struct Ser {
    enum class K { u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, boolean, f32, f64, string, bytes, pubkey,
                   option, vec, array, tuple, strct, enm };
    K k = K::u8;
    const Ser* elem = nullptr;
    std::size_t len = 0;
    std::vector<std::pair<std::string, const Ser*>> fields;  // struct
    std::vector<const Ser*> elems;                           // tuple
    std::vector<std::string> variants;                       // unit enum
    std::string name;
};

// Integer layouts and their refinement width; nullopt for every other layout.
std::optional<IntWidth> ser_width(Ser::K k) {
    switch (k) {
        case Ser::K::u8: return IntWidth::u8;
        case Ser::K::u16: return IntWidth::u16;
        case Ser::K::u32: return IntWidth::u32;
        case Ser::K::u64: return IntWidth::u64;
        case Ser::K::u128: return IntWidth::u128;
        case Ser::K::i8: return IntWidth::i8;
        case Ser::K::i16: return IntWidth::i16;
        case Ser::K::i32: return IntWidth::i32;
        case Ser::K::i64: return IntWidth::i64;
        case Ser::K::i128: return IntWidth::i128;
        case Ser::K::boolean: case Ser::K::f32: case Ser::K::f64: case Ser::K::string: case Ser::K::bytes:
        case Ser::K::pubkey: case Ser::K::option: case Ser::K::vec: case Ser::K::array: case Ser::K::tuple:
        case Ser::K::strct: case Ser::K::enm: break;
    }
    return std::nullopt;
}
bool is_int(Ser::K k) { return ser_width(k).has_value(); }
bool is_signed(Ser::K k) { return k == Ser::K::i8 || k == Ser::K::i16 || k == Ser::K::i32 || k == Ser::K::i64 || k == Ser::K::i128; }
int int_width_bytes(Ser::K k) {
    switch (k) {
        case Ser::K::u8: case Ser::K::i8: return 1;
        case Ser::K::u16: case Ser::K::i16: return 2;
        case Ser::K::u32: case Ser::K::i32: return 4;
        case Ser::K::u64: case Ser::K::i64: return 8;
        case Ser::K::u128: case Ser::K::i128: return 16;
        default: throw std::logic_error("int_width_bytes: not an integer layout");
    }
}
Ser::K ser_kind(IntWidth w) {
    switch (w) {
        case IntWidth::u8: return Ser::K::u8;
        case IntWidth::u16: return Ser::K::u16;
        case IntWidth::u32: return Ser::K::u32;
        case IntWidth::u64: return Ser::K::u64;
        case IntWidth::u128: return Ser::K::u128;
        case IntWidth::i8: return Ser::K::i8;
        case IntWidth::i16: return Ser::K::i16;
        case IntWidth::i32: return Ser::K::i32;
        case IntWidth::i64: return Ser::K::i64;
        case IntWidth::any:  // unrefined ints are 128-bit at runtime
        case IntWidth::i128: return Ser::K::i128;
    }
    return Ser::K::i128;
}

std::string snake_case(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (std::isupper(static_cast<unsigned char>(c))) {
            if (i > 0 && s[i - 1] != '_') out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

// IDL names may be camelCase (pre-0.30 Anchor) while scripts use snake_case.
bool same_name(std::string_view a, std::string_view b) { return a == b || snake_case(a) == snake_case(b); }

// Field `name` of `r`: an exact match wins, otherwise the first same_name match.
const Value* find_field(const RecordData& r, std::string_view name) {
    if (const Value* v = r.find(name)) return v;
    for (const auto& [n, v] : r.fields)
        if (same_name(n, name)) return &v;
    return nullptr;
}

// Name of the first field of `r` that matches none of `known`, if any.
const std::string* first_unknown_field(const RecordData& r, const std::vector<std::string>& known) {
    for (const auto& [n, _] : r.fields)
        if (std::none_of(known.begin(), known.end(), [&](const std::string& k) { return same_name(k, n); })) return &n;
    return nullptr;
}

// Checked access into untrusted JSON (IDL files): const operator[] on a
// missing key is undefined behaviour in nlohmann::json.
const Json& json_member(const Json& j, const char* key, Loc loc, const std::string& ctx) {
    if (!j.is_object() || !j.contains(key)) fail(loc, "malformed IDL: " + ctx + " has no \"" + key + "\"");
    return j[key];
}

void write_discriminator(sol::BorshWriter& w, const Json& d, Loc loc) {
    if (!d.is_array() || d.size() != 8) fail(loc, "malformed IDL: a discriminator must be an array of 8 bytes");
    for (const auto& b : d) {
        if (!b.is_number_unsigned() || b.get<std::uint64_t>() > 255) fail(loc, "malformed IDL: discriminator byte out of range");
        w.u8(static_cast<std::uint8_t>(b.get<std::uint64_t>()));
    }
}

class Codec {
public:
    const Ser* from_type(const sema::Type* t) {
        Ser s;
        switch (t->kind) {
            case TypeKind::int_: s.k = ser_kind(t->width); break;
            case TypeKind::bool_: s.k = Ser::K::boolean; break;
            case TypeKind::float_: s.k = t->is_f32 ? Ser::K::f32 : Ser::K::f64; break;
            case TypeKind::string: s.k = Ser::K::string; break;
            case TypeKind::bytes: s.k = Ser::K::bytes; break;
            case TypeKind::pubkey: s.k = Ser::K::pubkey; break;
            case TypeKind::option: s.k = Ser::K::option; s.elem = from_type(t->elem); break;
            case TypeKind::list:
                s.k = t->fixed_len ? Ser::K::array : Ser::K::vec;
                s.len = t->fixed_len.value_or(0);
                s.elem = from_type(t->elem);
                break;
            case TypeKind::tuple:
                s.k = Ser::K::tuple;
                for (const auto* e : t->elems) s.elems.push_back(from_type(e));
                break;
            case TypeKind::record:
                s.k = Ser::K::strct;
                s.name = t->schema ? t->schema->name : "";
                for (const auto& [n, ft] : t->fields) s.fields.emplace_back(n, from_type(ft));
                break;
            default: throw std::logic_error("type has no binary layout: " + sema::to_string(t));
        }
        return add(std::move(s));
    }

    // Anchor IDL type JSON -> layout. Supports both pre-0.30 and 0.30+ IDLs.
    const Ser* from_idl(const Json& idl, const Json& t, Loc loc, int depth = 0) {
        if (depth > 64) fail(loc, "IDL type nesting is too deep (recursive type?)");
        Ser s;
        if (t.is_string()) {
            static const std::pair<std::string_view, Ser::K> prims[] = {
                {"u8", Ser::K::u8},     {"u16", Ser::K::u16},   {"u32", Ser::K::u32},       {"u64", Ser::K::u64},
                {"u128", Ser::K::u128}, {"i8", Ser::K::i8},     {"i16", Ser::K::i16},       {"i32", Ser::K::i32},
                {"i64", Ser::K::i64},   {"i128", Ser::K::i128}, {"bool", Ser::K::boolean},  {"f32", Ser::K::f32},
                {"f64", Ser::K::f64},   {"string", Ser::K::string}, {"bytes", Ser::K::bytes},
                {"publicKey", Ser::K::pubkey}, {"pubkey", Ser::K::pubkey},
            };
            auto name = t.get<std::string>();
            for (auto [n, k] : prims)
                if (n == name) {
                    s.k = k;
                    return add(std::move(s));
                }
            fail(loc, "unsupported IDL type '" + name + "'");
        }
        if (t.is_object()) {
            if (t.contains("vec")) {
                s.k = Ser::K::vec;
                s.elem = from_idl(idl, t["vec"], loc, depth + 1);
                return add(std::move(s));
            }
            if (t.contains("option") || t.contains("coption")) {
                s.k = Ser::K::option;
                s.elem = from_idl(idl, t.contains("option") ? t["option"] : t["coption"], loc, depth + 1);
                return add(std::move(s));
            }
            if (t.contains("array")) {
                s.k = Ser::K::array;
                const Json& arr = t["array"];
                if (!arr.is_array() || arr.size() != 2) fail(loc, "malformed IDL: \"array\" must be [type, length]");
                s.elem = from_idl(idl, arr[0], loc, depth + 1);
                if (!arr[1].is_number_unsigned()) fail(loc, "unsupported generic IDL array length");
                s.len = arr[1].get<std::size_t>();
                return add(std::move(s));
            }
            if (t.contains("defined")) {
                const Json& d = t["defined"];
                std::string name = d.is_string() ? d.get<std::string>() : d.value("name", "");
                return defined(idl, name, loc, depth + 1);
            }
        }
        fail(loc, "unsupported IDL type " + t.dump());
    }

    const Ser* defined(const Json& idl, const std::string& name, Loc loc, int depth = 0) {
        for (const char* section : {"types", "accounts"}) {
            if (!idl.contains(section)) continue;
            for (const auto& def : idl[section]) {
                if (def.value("name", "") != name || !def.contains("type")) continue;
                return from_idl_def(idl, def, loc, depth);
            }
        }
        fail(loc, "IDL does not define type '" + name + "'");
    }

    void encode(sol::BorshWriter& w, const Ser& s, const Value& v, Loc loc, const std::string& path) const {
        auto mismatch = [&](const char* expected) {
            fail(loc, "cannot encode " + path + ": expected " + expected + ", found " + format_value(v, true));
        };
        if (auto width = ser_width(s.k)) {
            if (!v.is<__int128>()) mismatch("an integer");
            __int128 x = v.as<__int128>();
            auto [lo, hi] = sema::int_bounds(*width);
            bool fits = s.k == Ser::K::u128 ? x >= 0 : (x >= lo && x <= hi);
            if (!fits) fail(loc, "cannot encode " + path + ": " + int_to_string(x) + " is out of range for " + sema::to_string(*width));
            w.uint_le(static_cast<unsigned __int128>(x), int_width_bytes(s.k));
            return;
        }
        switch (s.k) {
            case Ser::K::boolean:
                if (!v.is<bool>()) mismatch("a bool");
                w.boolean(v.as<bool>());
                return;
            case Ser::K::f32:
            case Ser::K::f64: {
                double d;
                if (v.is<double>()) d = v.as<double>();
                else if (v.is<__int128>()) d = static_cast<double>(v.as<__int128>());
                else mismatch("a number");
                if (s.k == Ser::K::f32) w.f32(static_cast<float>(d));
                else w.f64(d);
                return;
            }
            case Ser::K::string:
                if (!v.is<std::string>()) mismatch("a string");
                w.string(v.as<std::string>());
                return;
            case Ser::K::bytes: {
                Bytes b = to_bytes(v, loc, path);
                w.bytes(b);
                return;
            }
            case Ser::K::pubkey:
                if (!v.is<Pubkey>()) mismatch("a pubkey");
                w.pubkey(v.as<Pubkey>());
                return;
            case Ser::K::option:
                if (v.is<Null>()) {
                    w.u8(0);
                    return;
                }
                w.u8(1);
                encode(w, *s.elem, v, loc, path);
                return;
            case Ser::K::vec:
            case Ser::K::array: {
                if (s.elem->k == Ser::K::u8 && v.is<Bytes>()) {
                    const Bytes& b = v.as<Bytes>();
                    if (s.k == Ser::K::array && b.size() != s.len)
                        fail(loc, "cannot encode " + path + ": expected " + std::to_string(s.len) + " bytes, found " + std::to_string(b.size()));
                    if (s.k == Ser::K::vec) w.u32(static_cast<std::uint32_t>(b.size()));
                    w.raw(b);
                    return;
                }
                if (!v.is<ListP>()) mismatch("a list");
                const auto& l = *v.as<ListP>();
                if (s.k == Ser::K::array && l.size() != s.len)
                    fail(loc, "cannot encode " + path + ": expected " + std::to_string(s.len) + " elements, found " + std::to_string(l.size()));
                if (s.k == Ser::K::vec) w.u32(static_cast<std::uint32_t>(l.size()));
                for (std::size_t i = 0; i < l.size(); ++i) encode(w, *s.elem, l[i], loc, path + "[" + std::to_string(i) + "]");
                return;
            }
            case Ser::K::tuple: {
                if (!v.is<ListP>()) mismatch("a tuple (list)");
                const auto& l = *v.as<ListP>();
                if (l.size() != s.elems.size()) fail(loc, "cannot encode " + path + ": wrong tuple arity");
                for (std::size_t i = 0; i < l.size(); ++i) encode(w, *s.elems[i], l[i], loc, path + "." + std::to_string(i));
                return;
            }
            case Ser::K::strct: {
                if (!v.is<RecordP>()) mismatch("an object");
                const auto& r = *v.as<RecordP>();
                std::vector<std::string> known;
                for (const auto& [fn, _] : s.fields) known.push_back(fn);
                if (const std::string* n = first_unknown_field(r, known))
                    fail(loc, "cannot encode " + path + ": unknown field '" + *n + "'" + (s.name.empty() ? "" : " for " + s.name));
                for (const auto& [fn, ft] : s.fields) {
                    const Value* fv = find_field(r, fn);
                    if (!fv) {
                        if (ft->k == Ser::K::option) {
                            w.u8(0);
                            continue;
                        }
                        fail(loc, "cannot encode " + path + ": missing field '" + fn + "'" + (s.name.empty() ? "" : " for " + s.name));
                    }
                    encode(w, *ft, *fv, loc, path + "." + fn);
                }
                return;
            }
            case Ser::K::enm: {
                if (!v.is<std::string>()) mismatch("an enum variant name (string)");
                const auto& name = v.as<std::string>();
                for (std::size_t i = 0; i < s.variants.size(); ++i)
                    if (same_name(s.variants[i], name)) {
                        w.u8(static_cast<std::uint8_t>(i));
                        return;
                    }
                fail(loc, "cannot encode " + path + ": '" + name + "' is not a variant of " + s.name);
            }
            default: break;
        }
        fail(loc, "cannot encode " + path);
    }

    // Smallest encoding of a value of layout `s`, to bound untrusted lengths.
    static std::size_t min_size(const Ser& s) {
        if (is_int(s.k)) return static_cast<std::size_t>(int_width_bytes(s.k));
        switch (s.k) {
            case Ser::K::boolean: case Ser::K::option: case Ser::K::enm: return 1;
            case Ser::K::f32: case Ser::K::string: case Ser::K::bytes: case Ser::K::vec: return 4;
            case Ser::K::f64: return 8;
            case Ser::K::pubkey: return 32;
            case Ser::K::array: return s.len * min_size(*s.elem);
            case Ser::K::tuple: {
                std::size_t n = 0;
                for (const auto* e : s.elems) n += min_size(*e);
                return n;
            }
            case Ser::K::strct: {
                std::size_t n = 0;
                for (const auto& [_, t] : s.fields) n += min_size(*t);
                return n;
            }
            default: return 0;
        }
    }

    Value decode(sol::BorshReader& r, const Ser& s) const {
        if (is_int(s.k)) {
            int n = int_width_bytes(s.k);
            unsigned __int128 u = r.uint_le(n);
            if (is_signed(s.k) && n < 16 && (u >> (8 * n - 1)) & 1) u |= ~static_cast<unsigned __int128>(0) << (8 * n);
            return Value(static_cast<__int128>(u));
        }
        switch (s.k) {
            case Ser::K::boolean: return Value(r.boolean());
            case Ser::K::f32: return Value(static_cast<double>(r.f32()));
            case Ser::K::f64: return Value(r.f64());
            case Ser::K::string: return Value(r.string());
            case Ser::K::bytes: return Value(r.bytes());
            case Ser::K::pubkey: return Value(r.pubkey());
            case Ser::K::option: return r.boolean() ? decode(r, *s.elem) : Value(Null{});
            case Ser::K::vec:
            case Ser::K::array: {
                std::size_t n = s.k == Ser::K::vec ? r.u32() : s.len;
                std::size_t each = min_size(*s.elem);
                if (each == 0 ? n > (1u << 16) : n > r.remaining() / each)
                    throw sol::DecodeError("length " + std::to_string(n) + " exceeds the available data");
                if (s.elem->k == Ser::K::u8) return Value(r.raw(n));
                std::vector<Value> items;
                for (std::size_t i = 0; i < n; ++i) items.push_back(decode(r, *s.elem));
                return make_list(std::move(items));
            }
            case Ser::K::tuple: {
                std::vector<Value> items;
                for (const auto* e : s.elems) items.push_back(decode(r, *e));
                return make_list(std::move(items));
            }
            case Ser::K::strct: {
                std::vector<std::pair<std::string, Value>> fields;
                for (const auto& [n, t] : s.fields) fields.emplace_back(n, decode(r, *t));
                return make_record(std::move(fields));
            }
            case Ser::K::enm: {
                auto i = r.u8();
                if (i >= s.variants.size()) throw sol::DecodeError("invalid enum variant");
                return Value(s.variants[i]);
            }
            default: break;
        }
        throw sol::DecodeError("unsupported layout");
    }

    static Bytes to_bytes(const Value& v, Loc loc, const std::string& what) {
        if (v.is<Bytes>()) return v.as<Bytes>();
        if (v.is<ListP>()) {
            Bytes out;
            for (const auto& e : *v.as<ListP>()) {
                if (!e.is<__int128>() || e.as<__int128>() < 0 || e.as<__int128>() > 255)
                    fail(loc, what + ": list elements must be integers in 0..255");
                out.push_back(static_cast<std::uint8_t>(e.as<__int128>()));
            }
            return out;
        }
        fail(loc, what + ": expected bytes, found " + format_value(v, true));
    }

private:
    const Ser* from_idl_def(const Json& idl, const Json& def, Loc loc, int depth) {
        static const std::string ctx = "type definition";
        const Json& ty = json_member(def, "type", loc, ctx);
        if (!ty.is_object()) fail(loc, "malformed IDL: type definition is not an object");
        std::string kind = ty.value("kind", "struct");
        Ser s;
        s.name = def.value("name", "");
        if (kind == "struct") {
            s.k = Ser::K::strct;
            const Json fields = ty.value("fields", Json::array());
            for (const auto& f : fields) {
                if (!f.is_object()) fail(loc, "tuple structs in IDL type '" + s.name + "' are not supported");
                s.fields.emplace_back(f.value("name", ""), from_idl(idl, json_member(f, "type", loc, "field of '" + s.name + "'"), loc, depth + 1));
            }
        } else if (kind == "enum") {
            s.k = Ser::K::enm;
            for (const auto& variant : ty.value("variants", Json::array())) {
                if (variant.contains("fields")) fail(loc, "enum variants with data in IDL type '" + s.name + "' are not supported");
                s.variants.push_back(variant.value("name", ""));
            }
        } else {
            fail(loc, "unsupported IDL type kind '" + kind + "' for '" + s.name + "'");
        }
        return add(std::move(s));
    }

    const Ser* add(Ser s) {
        arena_.push_back(std::move(s));
        return &arena_.back();
    }
    std::deque<Ser> arena_;
};

// ============================================================ interpreter ==

enum class Flow { normal, brk, cont, ret };

struct Frame {
    Frame* parent = nullptr;
    std::unordered_map<const Symbol*, Value> vars;
};

constexpr std::uint64_t max_steps = 10'000'000;
constexpr int max_call_depth = 256;
constexpr auto await_poll_interval = std::chrono::milliseconds(400);
constexpr std::size_t max_lookup_table_addresses = 256;

}  // namespace

struct Interpreter::Impl {
    const sema::Program& prog;
    Diagnostics& diags;
    std::ostream& out;
    Codec codec;

    std::unordered_map<const Symbol*, Value> globals;
    Frame* frame = nullptr;
    Value ret_value;
    std::uint64_t steps = 0;
    int depth = 0;

    backend::GenesisBuilder* genesis = nullptr;
    backend::RuntimeClient* runtime = nullptr;
    std::int64_t confirm_timeout_ms = backend::default_confirm_timeout_ms;

    // What this run's genesis declared: drives cross-declaration validation
    // and runtime IDL lookups. Reset as a whole at the start of every run.
    struct GenesisLedger {
        struct Declared {
            Loc loc;
            std::string kind;
        };
        struct MintInfo {
            Pubkey token_program;
            std::uint64_t supply;
            Loc loc;
        };
        struct TokenInfo {
            Pubkey address, mint, token_program;
            std::uint64_t amount;
            Loc loc;
        };
        std::map<Pubkey, Declared> declared;
        std::map<Pubkey, MintInfo> mints;
        std::vector<TokenInfo> token_accounts;
        std::map<Pubkey, std::filesystem::path> program_idls;
    } ledger;
    std::map<std::filesystem::path, Json> idl_cache;

    Impl(const sema::Program& p, Diagnostics& d, std::ostream& o) : prog(p), diags(d), out(o) {}

    std::filesystem::path base_dir() const { return prog.root()->dir; }

    std::filesystem::path resolve_path(const std::string& p) const {
        std::filesystem::path path(p);
        if (!p.empty() && p[0] == '~') {
            if (const char* home = std::getenv("HOME")) path = std::filesystem::path(home) / p.substr(p.size() > 1 && p[1] == '/' ? 2 : 1);
        }
        return path.is_absolute() ? path : base_dir() / path;
    }

    void step(Loc loc) {
        if (++steps > max_steps) fail(loc, "execution step limit exceeded (" + std::to_string(max_steps) + " steps); is there an infinite loop?");
    }

    // ------------------------------------------------------------ env --

    Value& slot(const Symbol* s, Loc loc) {
        for (Frame* f = frame; f; f = f->parent)
            if (auto it = f->vars.find(s); it != f->vars.end()) return it->second;
        if (auto it = globals.find(s); it != globals.end()) return it->second;
        fail(loc, "internal error: '" + s->name + "' used before initialization");
    }

    void bind(const Symbol* s, Value v) {
        if (s->is_global || !frame) globals[s] = std::move(v);
        else frame->vars[s] = std::move(v);
    }

    // --------------------------------------------------------- coercion --

    Value coerce(Value v, const sema::Type* to, Loc loc, const std::string& what) {
        if (!to) return v;
        switch (to->kind) {
            case TypeKind::float_:
                if (v.is<__int128>()) return Value(static_cast<double>(v.as<__int128>()));
                return v;
            case TypeKind::int_: {
                if (v.is<__int128>() && to->width != IntWidth::any) {
                    auto [lo, hi] = sema::int_bounds(to->width);
                    __int128 x = v.as<__int128>();
                    if (x < lo || x > hi)
                        fail(loc, what + ": " + int_to_string(x) + " is out of range for " + sema::to_string(to->width));
                }
                return v;
            }
            case TypeKind::bytes:
                if (v.is<ListP>()) return Value(Codec::to_bytes(v, loc, what));
                return v;
            case TypeKind::option:
                if (v.is<Null>()) return v;
                return coerce(std::move(v), to->elem, loc, what);
            case TypeKind::list:
                if (to->fixed_len) {
                    std::size_t n = v.is<Bytes>() ? v.as<Bytes>().size() : v.is<ListP>() ? v.as<ListP>()->size() : *to->fixed_len;
                    if (n != *to->fixed_len)
                        fail(loc, what + ": expected " + std::to_string(*to->fixed_len) + " elements, found " + std::to_string(n));
                }
                if (v.is<Bytes>() && to->elem->kind == TypeKind::int_) {
                    std::vector<Value> items;
                    for (auto b : v.as<Bytes>()) items.emplace_back(static_cast<__int128>(b));
                    return make_list(std::move(items));
                }
                if (v.is<ListP>() && (to->elem->kind == TypeKind::float_ || to->elem->kind == TypeKind::bytes ||
                                      to->elem->kind == TypeKind::int_ || to->elem->kind == TypeKind::option ||
                                      to->elem->kind == TypeKind::list || to->elem->kind == TypeKind::record)) {
                    std::vector<Value> items;
                    for (const auto& e : *v.as<ListP>()) items.push_back(coerce(e, to->elem, loc, what));
                    return make_list(std::move(items));
                }
                return v;
            case TypeKind::record:
                if (v.is<RecordP>() && !to->fields.empty()) {
                    auto r = std::make_shared<RecordData>(*v.as<RecordP>());
                    for (auto& [n, fv] : r->fields)
                        if (const sema::Type* ft = to->field(n)) fv = coerce(fv, ft, loc, what + "." + n);
                    // Optional schema fields that were omitted become null.
                    for (const auto& [n, ft] : to->fields)
                        if (!r->find(n) && ft->kind == TypeKind::option) r->fields.emplace_back(n, Value(Null{}));
                    return Value(std::move(r));
                }
                return v;
            default: return v;
        }
    }

    // ------------------------------------------------------ conversions --

    template <typename T>
    const T& want(const Value& v, Loc loc, const char* type_name, const std::string& what) {
        if (!v.is<T>()) fail(loc, what + ": expected " + type_name + ", found " + format_value(v, true));
        return v.as<T>();
    }
    __int128 to_int(const Value& v, Loc loc, const std::string& what) { return want<__int128>(v, loc, "an integer", what); }
    std::uint64_t to_u64(const Value& v, Loc loc, const std::string& what) {
        __int128 x = to_int(v, loc, what);
        if (x < 0 || x > static_cast<__int128>(UINT64_MAX)) fail(loc, what + ": " + int_to_string(x) + " is out of range for u64");
        return static_cast<std::uint64_t>(x);
    }
    std::uint32_t to_u32(const Value& v, Loc loc, const std::string& what) {
        __int128 x = to_int(v, loc, what);
        if (x < 0 || x > static_cast<__int128>(UINT32_MAX)) fail(loc, what + ": " + int_to_string(x) + " is out of range for u32");
        return static_cast<std::uint32_t>(x);
    }
    std::uint8_t to_u8(const Value& v, Loc loc, const std::string& what) {
        __int128 x = to_int(v, loc, what);
        if (x < 0 || x > 255) fail(loc, what + ": " + int_to_string(x) + " is out of range for u8");
        return static_cast<std::uint8_t>(x);
    }
    // By value: callers often convert temporaries returned by eval().
    Pubkey to_pubkey(const Value& v, Loc loc, const std::string& what) { return want<Pubkey>(v, loc, "a pubkey", what); }
    std::string to_str(const Value& v, Loc loc, const std::string& what) { return want<std::string>(v, loc, "a string", what); }
    std::optional<Pubkey> to_opt_pubkey(const Value& v, Loc loc, const std::string& what) {
        if (v.is<Null>()) return std::nullopt;
        return to_pubkey(v, loc, what);
    }
    double to_double(const Value& v, Loc loc, const std::string& what) {
        if (v.is<double>()) return v.as<double>();
        if (v.is<__int128>()) return static_cast<double>(v.as<__int128>());
        fail(loc, what + ": expected a number, found " + format_value(v, true));
    }

    // ------------------------------------------------------ expressions --

    Value eval(const Expr& e) {
        return std::visit([&](const auto& n) { return eval_node(e, n); }, e.node);
    }

    Value eval_node(const Expr& e, const NumberLit& n) {
        if (!n.folded_int) return Value(n.float_value);
        if (e.type && e.type->kind == TypeKind::duration) return Value(Duration{static_cast<std::int64_t>(n.int_value)});
        return Value(n.int_value);
    }
    Value eval_node(const Expr&, const StringLit& s) { return Value(s.value); }
    Value eval_node(const Expr&, const BoolLit& b) { return Value(b.value); }
    Value eval_node(const Expr&, const NullLit&) { return Value(Null{}); }

    Value symbol_value(const Symbol* s, Loc loc) {
        if (s->kind == SymbolKind::schema) return Value(SchemaV{s});
        if (s->kind == SymbolKind::module) return Value(ModuleV{s->module});
        return slot(s, loc);
    }

    Value eval_node(const Expr& e, const IdentExpr& id) { return symbol_value(id.sym, e.loc); }

    Value eval_node(const Expr& e, const ListLit& l) {
        std::vector<Value> items;
        items.reserve(l.elems.size());
        for (const auto& el : l.elems) items.push_back(eval(*el));
        Value v = make_list(std::move(items));
        return e.type ? coerce(std::move(v), e.type, e.loc, "list") : v;
    }

    Value eval_node(const Expr&, const ObjectLit& o) {
        std::vector<std::pair<std::string, Value>> fields;
        for (const auto& f : o.fields) fields.emplace_back(f.key, eval(*f.value));
        return make_record(std::move(fields));
    }

    Value eval_node(const Expr& e, const UnaryExpr& u) {
        Value v = eval(*u.operand);
        if (u.op == UnaryOp::not_) return Value(!want<bool>(v, e.loc, "a bool", "operand of '!'"));
        if (v.is<__int128>()) {
            if (v.as<__int128>() == std::numeric_limits<__int128>::min()) fail(e.loc, "integer overflow");
            return Value(-v.as<__int128>());
        }
        if (v.is<double>()) return Value(-v.as<double>());
        if (v.is<Duration>()) return Value(Duration{-v.as<Duration>().ms});
        fail(e.loc, "operand of '-' must be a number");
    }

    Value arith(BinaryOp op, const Value& a, const Value& b, Loc loc) {
        if (a.is<__int128>() && b.is<__int128>()) {
            __int128 x = a.as<__int128>(), y = b.as<__int128>(), r = 0;
            bool overflow = false;
            switch (op) {
                case BinaryOp::add: overflow = __builtin_add_overflow(x, y, &r); break;
                case BinaryOp::sub: overflow = __builtin_sub_overflow(x, y, &r); break;
                case BinaryOp::mul: overflow = __builtin_mul_overflow(x, y, &r); break;
                case BinaryOp::div:
                case BinaryOp::mod:
                    if (y == 0) fail(loc, op == BinaryOp::div ? "division by zero" : "modulo by zero");
                    if (x == std::numeric_limits<__int128>::min() && y == -1) overflow = true;
                    else r = op == BinaryOp::div ? x / y : x % y;
                    break;
                default: break;
            }
            if (overflow) fail(loc, "integer overflow");
            return Value(r);
        }
        if ((a.is<__int128>() || a.is<double>()) && (b.is<__int128>() || b.is<double>())) {
            double x = to_double(a, loc, "operand"), y = to_double(b, loc, "operand");
            switch (op) {
                case BinaryOp::add: return Value(x + y);
                case BinaryOp::sub: return Value(x - y);
                case BinaryOp::mul: return Value(x * y);
                case BinaryOp::div:
                    if (y == 0) fail(loc, "division by zero");
                    return Value(x / y);
                case BinaryOp::mod:
                    if (y == 0) fail(loc, "modulo by zero");
                    return Value(std::fmod(x, y));
                default: break;
            }
        }
        if (a.is<Duration>() || b.is<Duration>()) {
            auto ms = [](const Value& v) -> __int128 { return v.is<Duration>() ? v.as<Duration>().ms : v.as<__int128>(); };
            auto dur = [&](__int128 v) {
                if (v < INT64_MIN || v > INT64_MAX) fail(loc, "duration overflow");
                return Value(Duration{static_cast<std::int64_t>(v)});
            };
            bool both = a.is<Duration>() && b.is<Duration>();
            bool scale = (a.is<Duration>() && b.is<__int128>()) || (a.is<__int128>() && b.is<Duration>());
            __int128 x = ms(a), y = ms(b), r = 0;
            if (both && op == BinaryOp::add) return dur(x + y);
            if (both && op == BinaryOp::sub) return dur(x - y);
            if (scale && op == BinaryOp::mul) {
                if (__builtin_mul_overflow(x, y, &r)) fail(loc, "duration overflow");
                return dur(r);
            }
            if (a.is<Duration>() && b.is<__int128>() && op == BinaryOp::div) {
                if (y == 0) fail(loc, "division by zero");
                return dur(x / y);
            }
        }
        if (op == BinaryOp::add) {
            if (a.is<std::string>() && b.is<std::string>()) return Value(a.as<std::string>() + b.as<std::string>());
            if (a.is<Bytes>() && b.is<Bytes>()) {
                Bytes r = a.as<Bytes>();
                r.insert(r.end(), b.as<Bytes>().begin(), b.as<Bytes>().end());
                return Value(std::move(r));
            }
            if (a.is<ListP>() && b.is<ListP>()) {
                std::vector<Value> r = *a.as<ListP>();
                r.insert(r.end(), b.as<ListP>()->begin(), b.as<ListP>()->end());
                return make_list(std::move(r));
            }
        }
        fail(loc, "invalid operands " + format_value(a, true) + " and " + format_value(b, true));
    }

    int compare(const Value& a, const Value& b, Loc loc) {
        if (a.is<__int128>() && b.is<__int128>()) return a.as<__int128>() < b.as<__int128>() ? -1 : a.as<__int128>() > b.as<__int128>();
        if (a.is<Duration>() && b.is<Duration>()) return a.as<Duration>().ms < b.as<Duration>().ms ? -1 : a.as<Duration>().ms > b.as<Duration>().ms;
        if (a.is<std::string>() && b.is<std::string>()) return a.as<std::string>().compare(b.as<std::string>());
        double x = to_double(a, loc, "operand"), y = to_double(b, loc, "operand");
        return x < y ? -1 : x > y;
    }

    Value eval_node(const Expr&, const BinaryExpr& b) {
        Loc loc = b.op_loc;
        switch (b.op) {
            case BinaryOp::and_:
                return Value(want<bool>(eval(*b.lhs), loc, "a bool", "operand of '&&'") &&
                             want<bool>(eval(*b.rhs), loc, "a bool", "operand of '&&'"));
            case BinaryOp::or_:
                return Value(want<bool>(eval(*b.lhs), loc, "a bool", "operand of '||'") ||
                             want<bool>(eval(*b.rhs), loc, "a bool", "operand of '||'"));
            default: break;
        }
        Value l = eval(*b.lhs);
        Value r = eval(*b.rhs);
        switch (b.op) {
            case BinaryOp::eq: return Value(values_equal(l, r));
            case BinaryOp::ne: return Value(!values_equal(l, r));
            case BinaryOp::lt: return Value(compare(l, r, loc) < 0);
            case BinaryOp::le: return Value(compare(l, r, loc) <= 0);
            case BinaryOp::gt: return Value(compare(l, r, loc) > 0);
            case BinaryOp::ge: return Value(compare(l, r, loc) >= 0);
            case BinaryOp::range:
            case BinaryOp::range_incl:
                return Value(RangeV{to_int(l, loc, "range start"), to_int(r, loc, "range end"), b.op == BinaryOp::range_incl});
            default: return arith(b.op, l, r, loc);
        }
    }

    Value member_of(const Value& obj, const std::string& name, Loc loc) {
        if (obj.is<sol::Keypair>() && name == "pubkey") return Value(obj.as<sol::Keypair>().pubkey);
        if (obj.is<SourceV>() && name == "url") return Value(obj.as<SourceV>().url);
        if (obj.is<RecordP>()) {
            if (const Value* f = obj.as<RecordP>()->find(name)) return *f;
            fail(loc, "object has no field '" + name + "'");
        }
        if (obj.is<Null>()) fail(loc, "cannot read member '" + name + "' of null");
        fail(loc, "value " + format_value(obj, true) + " has no member '" + name + "'");
    }

    Value eval_node(const Expr& e, const MemberExpr& m) {
        if (m.module_member) return symbol_value(m.module_member, e.loc);
        return member_of(eval(*m.object), m.member.name, m.member.loc);
    }

    // `i` as a position into a container of `size` elements, or a positioned error.
    static std::size_t checked_index(__int128 i, std::size_t size, Loc loc, const char* container) {
        if (i < 0 || i >= static_cast<__int128>(size))
            fail(loc, "index " + int_to_string(i) + " is out of bounds for " + container + " of length " + std::to_string(size));
        return static_cast<std::size_t>(i);
    }

    Value eval_node(const Expr& e, const IndexExpr& ix) {
        Value obj = eval(*ix.object);
        __int128 i = to_int(eval(*ix.index), ix.index->loc, "index");
        if (obj.is<ListP>()) {
            const auto& l = *obj.as<ListP>();
            return l[checked_index(i, l.size(), e.loc, "a list")];
        }
        if (obj.is<Bytes>()) {
            const auto& b = obj.as<Bytes>();
            return Value(static_cast<__int128>(b[checked_index(i, b.size(), e.loc, "bytes")]));
        }
        fail(e.loc, "value cannot be indexed");
    }

    Value eval_node(const Expr& e, const CallExpr& c) {
        std::vector<Value> args;
        args.reserve(c.args.size());
        for (const auto& a : c.args) args.push_back(eval(*a));
        if (c.function) return call_function(c.function, std::move(args), e.loc);
        return call_builtin(e, c, std::move(args));
    }

    Value call_function(const Symbol* fn, std::vector<Value> args, Loc loc) {
        if (depth >= max_call_depth) fail(loc, "maximum call depth (" + std::to_string(max_call_depth) + ") exceeded; is there unbounded recursion?");
        const FunctionDecl& decl = *fn->function;
        Frame callee;  // functions see globals and their parameters only
        for (std::size_t i = 0; i < args.size(); ++i)
            callee.vars[decl.params[i].sym] =
                coerce(std::move(args[i]), fn->param_types[i], loc, "argument '" + decl.params[i].name.name + "'");
        Flow f;
        {
            Restore in_callee(frame, &callee);
            Restore deeper(depth, depth + 1);
            f = exec_block(decl.body);
        }
        if (f == Flow::ret) {
            Value r = std::move(ret_value);
            ret_value = Value();
            return coerce(std::move(r), fn->return_type, loc, "return value of '" + fn->name + "'");
        }
        return Value();
    }

    // --------------------------------------------------------- builtins --

    std::optional<backend::AccountView> query_account(const Pubkey& pk) {
        if (runtime) return runtime->account(pk);
        if (genesis) return genesis->account(pk);
        throw std::logic_error("state query outside genesis/runtime");
    }

    std::vector<Bytes> seeds_of(const Value& v, Loc loc) {
        std::vector<Bytes> seeds;
        for (const auto& s : *want<ListP>(v, loc, "a list", "seeds")) {
            if (s.is<std::string>()) seeds.emplace_back(s.as<std::string>().begin(), s.as<std::string>().end());
            else if (s.is<Bytes>()) seeds.push_back(s.as<Bytes>());
            else if (s.is<Pubkey>()) seeds.emplace_back(s.as<Pubkey>().bytes.begin(), s.as<Pubkey>().bytes.end());
            else fail(loc, "seed must be a string, bytes or pubkey, found " + format_value(s, true));
            if (seeds.back().size() > sol::max_seed_len)
                fail(loc, "seed is " + std::to_string(seeds.back().size()) + " bytes; the maximum is 32");
        }
        if (seeds.size() > sol::max_seeds - 1) fail(loc, "too many seeds (maximum 15 plus the bump)");
        return seeds;
    }

    std::string read_text_file(const std::filesystem::path& p, Loc loc, const char* what) {
        std::ifstream in(p, std::ios::binary);
        if (!in) fail(loc, std::string("cannot read ") + what + " '" + p.string() + "'");
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    static Bytes le_bytes(__int128 v, int n) {
        sol::BorshWriter w;
        w.uint_le(static_cast<unsigned __int128>(v), n);
        return w.take();
    }

    const Symbol* find_schema_named(const std::string& name) {
        for (const auto& m : prog.modules)
            if (auto it = m->exports.find(name); it != m->exports.end() && it->second->kind == SymbolKind::schema)
                return it->second;
        return nullptr;
    }

    const Json& load_idl(const std::filesystem::path& path, Loc loc) {
        auto it = idl_cache.find(path);
        if (it != idl_cache.end()) return it->second;
        std::string text = read_text_file(path, loc, "IDL file");
        Json j = Json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) fail(loc, "IDL file '" + path.string() + "' is not valid JSON");
        return idl_cache.emplace(path, std::move(j)).first->second;
    }

    Bytes encode_anchor_account(const Value& schema_or_name, const Value& data, Loc loc) {
        if (schema_or_name.is<SchemaV>()) {
            const Symbol* s = schema_or_name.as<SchemaV>().sym;
            sol::BorshWriter w;
            auto disc = sol::anchor_discriminator("account", s->name);
            w.raw(disc);
            codec.encode(w, *codec.from_type(s->schema_record), coerce(data, s->schema_record, loc, s->name), loc, s->name);
            return w.take();
        }
        const std::string& name = to_str(schema_or_name, loc, "anchor account type");
        if (const Symbol* s = find_schema_named(name)) return encode_anchor_account(Value(SchemaV{s}), data, loc);
        for (const auto& [program, idl_path] : ledger.program_idls) {
            const Json& idl = load_idl(idl_path, loc);
            for (const auto& acc : idl.value("accounts", Json::array())) {
                if (acc.value("name", "") != name) continue;
                sol::BorshWriter w;
                if (acc.contains("discriminator")) {
                    write_discriminator(w, acc["discriminator"], loc);
                } else {
                    w.raw(sol::anchor_discriminator("account", name));
                }
                const Ser* layout = codec.defined(idl, name, loc);
                codec.encode(w, *layout, data, loc, name);
                return w.take();
            }
        }
        fail(loc, "unknown anchor account type '" + name + "': declare 'schema " + name +
                      " using anchor { ... }' or give the owning program an 'idl' in genesis");
    }

    // Typed, positioned access to a builtin call's evaluated arguments. Errors
    // point at the offending argument and name it ("argument 2 of 'ata'").
    struct BuiltinArgs {
        Impl& in;
        const CallExpr& call;
        const std::vector<Value>& values;
        Loc call_loc;
        std::string name;

        std::size_t size() const { return values.size(); }
        const Value& operator[](std::size_t i) const { return values[i]; }
        Loc loc(std::size_t i) const { return i < call.args.size() ? call.args[i]->loc : call_loc; }
        std::string what(std::size_t i) const { return "argument " + std::to_string(i + 1) + " of '" + name + "'"; }

        std::string str(std::size_t i) const { return in.to_str(values[i], loc(i), what(i)); }
        Pubkey pubkey(std::size_t i) const { return in.to_pubkey(values[i], loc(i), what(i)); }
        std::uint64_t u64(std::size_t i) const { return in.to_u64(values[i], loc(i), what(i)); }
        std::uint8_t u8(std::size_t i) const { return in.to_u8(values[i], loc(i), what(i)); }
        double number(std::size_t i) const { return in.to_double(values[i], loc(i), what(i)); }
        bool boolean(std::size_t i) const { return in.want<bool>(values[i], loc(i), "a bool", what(i)); }
        Bytes bytes(std::size_t i) const { return Codec::to_bytes(values[i], loc(i), what(i)); }
        const Symbol* schema(std::size_t i) const { return in.want<SchemaV>(values[i], loc(i), "a schema", what(i)).sym; }
        // An integer range-checked against `w`.
        __int128 int_as(std::size_t i, IntWidth w) const {
            return in.to_int(in.coerce(values[i], in.prog.types.int_(w), loc(i), what(i)), loc(i), what(i));
        }
    };

    Value call_builtin(const Expr& e, const CallExpr& c, std::vector<Value> args) {
        const Loc loc = e.loc;
        const BuiltinArgs a{*this, c, args, loc, sema::builtin_name(c.builtin)};
        switch (c.builtin) {
            case BuiltinId::seed: return Value(sol::Keypair::from_name(a.str(0)));
            case BuiltinId::keypair_file: {
                auto path = resolve_path(a.str(0));
                try {
                    return Value(sol::Keypair::from_json(read_text_file(path, a.loc(0), "keypair file")));
                } catch (const std::invalid_argument& ex) {
                    fail(a.loc(0), "invalid keypair file '" + path.string() + "': " + ex.what());
                }
            }
            case BuiltinId::pubkey: {
                std::string text = a.str(0);
                auto p = Pubkey::from_base58(text);
                if (!p) fail(a.loc(0), "invalid pubkey '" + text + "'");
                return Value(*p);
            }
            case BuiltinId::pda:
            case BuiltinId::pda_bump: {
                auto [addr, bump] = sol::find_program_address(seeds_of(a[1], a.loc(1)), a.pubkey(0));
                if (c.builtin == BuiltinId::pda) return Value(addr);
                return Value(static_cast<__int128>(bump));
            }
            case BuiltinId::ata: {
                Pubkey tp = a.size() > 2 ? a.pubkey(2) : sol::ids::token_program;
                return Value(sol::associated_token_address(a.pubkey(0), a.pubkey(1), tp));
            }
            case BuiltinId::system_program: return Value(sol::ids::system_program);
            case BuiltinId::token_program: return Value(sol::ids::token_program);
            case BuiltinId::token_2022_program: return Value(sol::ids::token_2022_program);
            case BuiltinId::associated_token_program: return Value(sol::ids::associated_token_program);
            case BuiltinId::bpf_loader: return Value(sol::ids::bpf_loader);
            case BuiltinId::bpf_loader_deprecated: return Value(sol::ids::bpf_loader_deprecated);
            case BuiltinId::bpf_upgradeable_loader: return Value(sol::ids::bpf_upgradeable_loader);
            case BuiltinId::loader_v4: return Value(sol::ids::loader_v4);
            case BuiltinId::memo_program: return Value(sol::ids::memo_program);
            case BuiltinId::compute_budget_program: return Value(sol::ids::compute_budget_program);
            case BuiltinId::address_lookup_table_program: return Value(sol::ids::address_lookup_table_program);
            case BuiltinId::hex:
            case BuiltinId::base64:
            case BuiltinId::base58: {
                std::string text = a.str(0);
                auto b = c.builtin == BuiltinId::hex      ? sol::hex_decode(text)
                         : c.builtin == BuiltinId::base64 ? sol::base64_decode(text)
                                                          : sol::base58_decode(text);
                if (!b) fail(a.loc(0), "invalid " + a.name + " string");
                return Value(std::move(*b));
            }
            case BuiltinId::utf8:
            case BuiltinId::memo: {
                std::string text = a.str(0);
                return Value(Bytes(text.begin(), text.end()));
            }
            case BuiltinId::sha256: {
                sol::Hash h = a[0].is<std::string>() ? sol::sha256(sol::as_bytes(a[0].as<std::string>())) : sol::sha256(a.bytes(0));
                return Value(Bytes(h.begin(), h.end()));
            }
            case BuiltinId::file: {
                std::string text = read_text_file(resolve_path(a.str(0)), a.loc(0), "file");
                return Value(Bytes(text.begin(), text.end()));
            }
            case BuiltinId::u8: return Value(Bytes{a.u8(0)});
            case BuiltinId::u16_le: return Value(le_bytes(a.int_as(0, IntWidth::u16), 2));
            case BuiltinId::u32_le: return Value(le_bytes(a.int_as(0, IntWidth::u32), 4));
            case BuiltinId::u64_le: return Value(le_bytes(a.u64(0), 8));
            case BuiltinId::i64_le: return Value(le_bytes(a.int_as(0, IntWidth::i64), 8));
            case BuiltinId::borsh: {
                const Symbol* s = a.schema(0);
                sol::BorshWriter w;
                if (s->schema_format == sema::SchemaFormat::anchor) w.raw(sol::anchor_discriminator("account", s->name));
                codec.encode(w, *codec.from_type(s->schema_record), coerce(a[1], s->schema_record, a.loc(1), s->name), a.loc(1), s->name);
                return Value(w.take());
            }
            case BuiltinId::anchor: return Value(encode_anchor_account(a[0], a[1], a.loc(1)));
            case BuiltinId::decode: {
                const Symbol* s = a.schema(0);
                const Bytes data = a.bytes(1);
                sol::BorshReader r(data);
                try {
                    if (s->schema_format == sema::SchemaFormat::anchor) {
                        Bytes disc = r.raw(8);
                        auto expect_disc = sol::anchor_discriminator("account", s->name);
                        if (!std::equal(disc.begin(), disc.end(), expect_disc.begin()))
                            fail(a.loc(1), "data does not start with the anchor discriminator of '" + s->name + "'");
                    }
                    return codec.decode(r, *codec.from_type(s->schema_record));
                } catch (const sol::DecodeError& ex) {
                    fail(a.loc(1), "cannot decode data as '" + s->name + "': " + ex.what());
                }
            }
            case BuiltinId::len:
                if (a[0].is<std::string>()) return Value(static_cast<__int128>(a[0].as<std::string>().size()));
                if (a[0].is<Bytes>()) return Value(static_cast<__int128>(a[0].as<Bytes>().size()));
                return Value(static_cast<__int128>(want<ListP>(a[0], a.loc(0), "a list", a.what(0))->size()));
            case BuiltinId::str: return Value(format_value(a[0]));
            case BuiltinId::print: {
                std::string line;
                for (std::size_t i = 0; i < a.size(); ++i) line += (i ? " " : "") + format_value(a[i]);
                out << line << "\n";
                out.flush();
                return Value();
            }
            case BuiltinId::rent_exempt: return Value(static_cast<__int128>(sol::rent_exempt_minimum(a.u64(0))));
            case BuiltinId::rpc: return Value(SourceV{a.str(0)});
            case BuiltinId::readonly: return Value(MetaV{a.pubkey(0), false, false});
            case BuiltinId::writable: return Value(MetaV{a.pubkey(0), false, true});
            case BuiltinId::signer: return Value(MetaV{a.pubkey(0), true, a.size() < 2 || a.boolean(1)});
            case BuiltinId::sol: {
                if (a[0].is<__int128>()) {
                    __int128 r;
                    if (__builtin_mul_overflow(a[0].as<__int128>(), static_cast<__int128>(sol::lamports_per_sol), &r))
                        fail(loc, "integer overflow");
                    return Value(r);
                }
                double d = a.number(0) * static_cast<double>(sol::lamports_per_sol);
                if (d != std::floor(d)) fail(a.loc(0), "amount is not a whole number of lamports");
                if (!(std::fabs(d) < 1.7e38)) fail(a.loc(0), "amount is out of range");  // __int128 holds about 1.7e38
                return Value(static_cast<__int128>(d));
            }
            case BuiltinId::system_transfer: return Value(sol::system_transfer_data(a.u64(0)));
            case BuiltinId::spl_transfer: return Value(sol::spl_transfer_data(a.u64(0)));
            case BuiltinId::spl_mint_to: return Value(sol::spl_mint_to_data(a.u64(0)));
            case BuiltinId::spl_transfer_checked: return Value(sol::spl_transfer_checked_data(a.u64(0), a.u8(1)));
            case BuiltinId::balance: {
                auto acc = query_account(a.pubkey(0));
                return Value(static_cast<__int128>(acc ? acc->lamports : 0));
            }
            case BuiltinId::token_balance: {
                Pubkey pk = a.pubkey(0);
                auto acc = query_account(pk);
                if (!acc) fail(a.loc(0), "token account " + pk.base58() + " does not exist");
                if (acc->owner != sol::ids::token_program && acc->owner != sol::ids::token_2022_program)
                    fail(a.loc(0), "account " + pk.base58() + " is not owned by a token program");
                auto t = sol::decode_token_account(acc->data);
                if (!t) fail(a.loc(0), "account " + pk.base58() + " is not a token account");
                return Value(static_cast<__int128>(t->amount));
            }
            case BuiltinId::account: {
                auto acc = query_account(a.pubkey(0));
                if (!acc) return Value(Null{});
                return make_record({{"address", Value(acc->address)},
                                    {"lamports", Value(static_cast<__int128>(acc->lamports))},
                                    {"owner", Value(acc->owner)},
                                    {"data", Value(acc->data)},
                                    {"executable", Value(acc->executable)},
                                    {"rent_epoch", Value(static_cast<__int128>(acc->rent_epoch))}});
            }
            case BuiltinId::exists: return Value(query_account(a.pubkey(0)).has_value());
            case BuiltinId::slot: return Value(static_cast<__int128>(runtime->slot()));
            case BuiltinId::epoch: return Value(static_cast<__int128>(runtime->epoch()));
            case BuiltinId::none: break;
        }
        fail(loc, "internal error: unknown builtin");
    }

    // ------------------------------------------------------- statements --

    Flow exec_block(const Block& b) {
        for (const auto& s : b) {
            Flow f = exec(*s);
            if (f != Flow::normal) return f;
        }
        return Flow::normal;
    }

    // Copy-on-write resolution of an assignment target.
    Value& lvalue(const Expr& e) {
        if (const auto* id = e.as<IdentExpr>()) return slot(id->sym, e.loc);
        if (const auto* m = e.as<MemberExpr>()) {
            Value& obj = lvalue(*m->object);
            if (!obj.is<RecordP>()) fail(e.loc, "cannot assign a member of a non-object value");
            auto& rp = obj.as<RecordP>();
            if (rp.use_count() > 1) rp = std::make_shared<RecordData>(*rp);
            if (Value* f = rp->find(m->member.name)) return *f;
            fail(m->member.loc, "object has no field '" + m->member.name + "'");
        }
        const auto& ix = *e.as<IndexExpr>();
        Value& obj = lvalue(*ix.object);
        __int128 i = to_int(eval(*ix.index), ix.index->loc, "index");
        if (obj.is<ListP>()) {
            auto& lp = obj.as<ListP>();
            if (lp.use_count() > 1) lp = std::make_shared<std::vector<Value>>(*lp);
            return (*lp)[checked_index(i, lp->size(), e.loc, "a list")];
        }
        fail(e.loc, "this value cannot be assigned by index");
    }

    void assign(const AssignStmt& a) {
        Value v = eval(*a.value);
        // Bytes elements are not addressable values; handle them directly.
        if (const auto* ix = a.target->as<IndexExpr>()) {
            Value& obj = lvalue(*ix->object);
            if (obj.is<Bytes>()) {
                __int128 i = to_int(eval(*ix->index), ix->index->loc, "index");
                Bytes& b = obj.as<Bytes>();
                std::size_t at = checked_index(i, b.size(), a.target->loc, "bytes");
                if (a.op != AssignOp::set) v = arith(compound_op(a.op), Value(static_cast<__int128>(b[at])), v, a.value->loc);
                b[at] = to_u8(v, a.value->loc, "byte value");
                return;
            }
        }
        Value& target = lvalue(*a.target);
        if (a.op != AssignOp::set) v = arith(compound_op(a.op), target, v, a.value->loc);
        target = coerce(std::move(v), a.target->type, a.value->loc, "assignment");
    }

    void exec_binding(const LetStmt& l) {
        Value v = eval(*l.init);
        if (l.kind == BindingKind::source && v.is<std::string>()) v = Value(SourceV{v.as<std::string>()});
        if (l.annotation) v = coerce(std::move(v), l.sym->type, l.init->loc, "initializer of '" + l.name.name + "'");
        bind(l.sym, std::move(v));
    }

    template <typename F>
    Flow scoped(F&& f) {
        Frame inner;
        inner.parent = frame;
        Restore in_inner(frame, &inner);
        return f();
    }

    // Library/backend exceptions (RPC failures, malformed JSON, ...) become
    // diagnostics at the innermost statement that raised them.
    Flow exec(const Stmt& s) {
        try {
            return exec_stmt(s);
        } catch (const TideError&) {
            throw;
        } catch (const std::exception& e) {
            throw TideError(s.loc, e.what());
        }
    }

    Flow exec_stmt(const Stmt& s) {
        step(s.loc);
        return std::visit(
            overloaded{
                [&](const LetStmt& l) {
                    exec_binding(l);
                    return Flow::normal;
                },
                [&](const AssignStmt& a) {
                    assign(a);
                    return Flow::normal;
                },
                [&](const IfStmt& i) {
                    bool c = want<bool>(eval(*i.cond), i.cond->loc, "a bool", "if condition");
                    if (c) return scoped([&] { return exec_block(i.then_block); });
                    if (i.else_block) return scoped([&] { return exec_block(*i.else_block); });
                    return Flow::normal;
                },
                [&](const ForStmt& f) { return exec_for(f); },
                [&](const WhileStmt& w) {
                    while (want<bool>(eval(*w.cond), w.cond->loc, "a bool", "while condition")) {
                        step(s.loc);
                        Flow r = scoped([&] { return exec_block(w.body); });
                        if (r == Flow::brk) break;
                        if (r == Flow::ret) return r;
                    }
                    return Flow::normal;
                },
                [&](const AssertStmt& a) {
                    if (!want<bool>(eval(*a.cond), a.cond->loc, "a bool", "assertion"))
                        fail(s.loc, "assertion failed" + (a.message ? ": " + *a.message : std::string()));
                    return Flow::normal;
                },
                [&](const ExprStmt& es) {
                    eval(*es.expr);
                    return Flow::normal;
                },
                [&](const BreakStmt&) { return Flow::brk; },
                [&](const ContinueStmt&) { return Flow::cont; },
                [&](const ReturnStmt& r) {
                    ret_value = r.value ? eval(*r.value) : Value();
                    return Flow::ret;
                },
                [&](const EntityDecl& d) {
                    exec_entity(s, d);
                    return Flow::normal;
                },
                [&](const SysvarDecl& d) {
                    exec_sysvar(s, d);
                    return Flow::normal;
                },
                [&](const FeatureDecl& f) {
                    genesis->set_feature({to_pubkey(eval(*f.feature), f.feature->loc, "feature id"), f.enabled, s.loc});
                    return Flow::normal;
                },
                [&](const ForkDecl& f) {
                    backend::ForkSpec spec{source_url(eval(*f.source)), std::nullopt, s.loc};
                    if (f.at_slot) spec.slot = to_u64(eval(*f.at_slot), f.at_slot->loc, "fork slot");
                    genesis->fork(spec);
                    return Flow::normal;
                },
                [&](const CloneDecl& c) {
                    exec_clone(s, c);
                    return Flow::normal;
                },
                [&](const DeleteDecl& d) {
                    exec_delete(s, d);
                    return Flow::normal;
                },
                [&](const TransactionStmt& t) {
                    exec_transaction(s, t);
                    return Flow::normal;
                },
                [&](const InvokeStmt& i) {
                    exec_invoke(s, i);
                    return Flow::normal;
                },
                [&](const DeployStmt& d) {
                    backend::DeployRequest req{want<sol::Keypair>(eval(*d.program), d.program->loc, "a keypair", "deployed program"),
                                               resolve_path(to_str(eval(*d.from), d.from->loc, "program binary path")), s.loc};
                    if (!std::filesystem::exists(req.binary)) fail(d.from->loc, "program binary '" + req.binary.string() + "' does not exist");
                    auto r = runtime->deploy(req);
                    if (!r.success) fail(s.loc, "deploy failed: " + r.error);
                    out << "    deployed " << req.program.pubkey.base58() << "\n";
                    return Flow::normal;
                },
                [&](const AirdropStmt& a) {
                    Pubkey to = to_pubkey(eval(*a.to), a.to->loc, "airdrop recipient");
                    std::uint64_t amount = to_u64(eval(*a.amount), a.amount->loc, "airdrop amount");
                    auto r = runtime->airdrop(to, amount, s.loc);
                    if (!r.success) fail(s.loc, "airdrop to " + to.base58() + " failed: " + r.error);
                    return Flow::normal;
                },
                [&](const WarpStmt&) -> Flow { fail(s.loc, "'warp' is not supported in this version of Tide"); },
                [&](const SnapshotStmt&) -> Flow { fail(s.loc, "'snapshot' is not supported in this version of Tide"); },
                [&](const RestoreStmt&) -> Flow { fail(s.loc, "'restore' is not supported in this version of Tide"); },
                [&](const AwaitStmt& a) {
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(confirm_timeout_ms);
                    while (!want<bool>(eval(*a.cond), a.cond->loc, "a bool", "await condition")) {
                        if (std::chrono::steady_clock::now() > deadline)
                            fail(s.loc, "await timed out after " + std::to_string(confirm_timeout_ms) + "ms");
                        std::this_thread::sleep_for(await_poll_interval);
                    }
                    return Flow::normal;
                },
            },
            s.node);
    }

    Flow exec_for(const ForStmt& f) {
        Value it = eval(*f.iterable);
        auto body = [&](Value v) {
            return scoped([&] {
                frame->vars[f.sym] = std::move(v);
                return exec_block(f.body);
            });
        };
        if (it.is<RangeV>()) {
            const auto& r = it.as<RangeV>();
            for (__int128 i = r.lo; r.inclusive ? i <= r.hi : i < r.hi; ++i) {
                step(f.var.loc);
                Flow fl = body(Value(i));
                if (fl == Flow::brk) break;
                if (fl == Flow::ret) return fl;
            }
            return Flow::normal;
        }
        ListP items = want<ListP>(it, f.iterable->loc, "a range or list", "for loop");  // holds a reference
        for (const auto& v : *items) {
            step(f.var.loc);
            Flow fl = body(v);
            if (fl == Flow::brk) break;
            if (fl == Flow::ret) return fl;
        }
        return Flow::normal;
    }

    // --------------------------------------------------------- genesis --

    std::string source_url(const Value& v) {
        if (v.is<SourceV>()) return v.as<SourceV>().url;
        return v.as<std::string>();
    }

    struct FieldVals {
        std::map<std::string, std::pair<Value, Loc>> values;
        struct Ext {
            std::string name;
            Loc loc;
            std::map<std::string, std::pair<Value, Loc>> values;
        };
        std::vector<Ext> extensions;

        const Value* get(const std::string& n) const {
            read.insert(n);
            auto it = values.find(n);
            return it == values.end() ? nullptr : &it->second.first;
        }
        // A field the checker guarantees (`required` in its sema field spec).
        // Missing means sema and the interpreter disagree: an internal error,
        // never a null dereference.
        const Value& require(const std::string& n) const {
            const Value* v = get(n);
            if (!v) throw std::logic_error("internal error: required field '" + n + "' is missing (sema field spec out of sync)");
            return *v;
        }
        // Every field the checker accepted must have been consumed by now.
        void check_all_read(std::string_view block) const {
            for (const auto& [n, _] : values)
                if (!read.count(n)) unhandled_key(block, n);
        }
        mutable std::set<std::string> read;
        Loc loc(const std::string& n, Loc fallback) const {
            auto it = values.find(n);
            return it == values.end() ? fallback : it->second.second;
        }
    };

    FieldVals eval_fields(const std::vector<RecordEntry>& entries) {
        FieldVals fv;
        for (const auto& e : entries) {
            if (e.kind == RecordEntry::Kind::value) fv.values[e.key] = {eval(*e.value), e.value->loc};
            else if (e.kind == RecordEntry::Kind::extension) {
                FieldVals::Ext ext{e.key, e.loc, {}};
                for (const auto& x : e.block->entries)
                    if (x.kind == RecordEntry::Kind::value) ext.values[x.key] = {eval(*x.value), x.value->loc};
                fv.extensions.push_back(std::move(ext));
            }
        }
        return fv;
    }

    void claim(const Pubkey& addr, Loc loc, const std::string& kind) {
        auto [it, ok] = ledger.declared.try_emplace(addr, GenesisLedger::Declared{loc, kind});
        if (!ok) {
            TideError err(loc, "address " + addr.base58() + " is already defined in genesis (as " + it->second.kind + ")");
            err.notes.push_back({it->second.loc, "previous definition is here"});
            throw err;
        }
    }

    std::uint64_t opt_u64(const FieldVals& f, const std::string& n, std::uint64_t dflt, Loc fallback) {
        const Value* v = f.get(n);
        return v ? to_u64(*v, f.loc(n, fallback), "field '" + n + "'") : dflt;
    }

    std::optional<Pubkey> opt_authority(const FieldVals& f, const std::string& n, Loc fallback) {
        const Value* v = f.get(n);
        return v ? to_opt_pubkey(*v, f.loc(n, fallback), "field '" + n + "'") : std::nullopt;
    }

    Pubkey token_program_of(const FieldVals& f, Loc fallback) {
        const Value* v = f.get("token_program");
        if (!v) return sol::ids::token_program;
        Pubkey tp = to_pubkey(*v, f.loc("token_program", fallback), "token_program");
        if (tp != sol::ids::token_program && tp != sol::ids::token_2022_program)
            fail(f.loc("token_program", fallback), "token_program must be token_program() or token_2022_program(), found " + tp.base58());
        return tp;
    }

    std::vector<sol::Extension> extensions_of(const FieldVals& f, const Pubkey& token_program) {
        std::vector<sol::Extension> exts;
        for (const auto& x : f.extensions) {
            if (token_program != sol::ids::token_2022_program)
                fail(x.loc, "extension '" + x.name + "' requires token_program = token_2022_program()");
            auto pk_or_zero = [&](const std::string& n) {
                Bytes b(32, 0);
                if (auto it = x.values.find(n); it != x.values.end())
                    if (auto p = to_opt_pubkey(it->second.first, it->second.second, n)) b.assign(p->bytes.begin(), p->bytes.end());
                return b;
            };
            sol::Extension e{};
            if (x.name == "mint_close_authority") e = {sol::ExtensionType::mint_close_authority, pk_or_zero("close_authority")};
            else if (x.name == "permanent_delegate") e = {sol::ExtensionType::permanent_delegate, pk_or_zero("delegate")};
            else if (x.name == "non_transferable") e = {sol::ExtensionType::non_transferable, {}};
            else if (x.name == "immutable_owner") e = {sol::ExtensionType::immutable_owner, {}};
            else if (x.name == "metadata_pointer") {
                Bytes b = pk_or_zero("authority");
                Bytes m = pk_or_zero("metadata_address");
                b.insert(b.end(), m.begin(), m.end());
                e = {sol::ExtensionType::metadata_pointer, b};
            } else if (x.name == "default_account_state") {
                const auto& [v, l] = x.values.at("state");
                e = {sol::ExtensionType::default_account_state, Bytes{static_cast<std::uint8_t>(token_state(v, l))}};
            } else fail(x.loc, "unsupported extension '" + x.name + "'");
            exts.push_back(std::move(e));
        }
        return exts;
    }

    sol::TokenAccountState token_state(const Value& v, Loc loc) {
        const std::string& s = to_str(v, loc, "token account state");
        if (s == "initialized") return sol::TokenAccountState::initialized;
        if (s == "frozen") return sol::TokenAccountState::frozen;
        fail(loc, "token account state must be \"initialized\" or \"frozen\", found \"" + s + "\"");
    }

    void put(const Pubkey& addr, std::uint64_t lamports, const Pubkey& owner, Bytes data, bool executable,
             std::uint64_t rent_epoch, Loc loc, const std::string& kind) {
        claim(addr, loc, kind);
        if (lamports == 0) diags.warning(loc, kind + " " + addr.base58() + " has 0 lamports and will not exist on chain");
        else if (lamports < sol::rent_exempt_minimum(data.size()))
            diags.warning(loc, kind + " " + addr.base58() + " holds " + std::to_string(lamports) +
                                   " lamports, below the rent-exempt minimum of " + std::to_string(sol::rent_exempt_minimum(data.size())) +
                                   " for " + std::to_string(data.size()) + " bytes");
        genesis->put_account({addr, lamports, owner, std::move(data), executable, rent_epoch, loc});
    }

    void exec_entity(const Stmt& s, const EntityDecl& d) {
        FieldVals f = eval_fields(d.fields);
        switch (d.kind) {
            case EntityKind::account: put_plain_account(d, f, s.loc); break;
            case EntityKind::program: put_program(d, f, s.loc); break;
            case EntityKind::mint: put_mint(d, f, s.loc); break;
            case EntityKind::token_account:
            case EntityKind::associated_token_account: put_token_account(d, f, s.loc); break;
            case EntityKind::lookup_table: put_lookup_table(d, f, s.loc); break;
        }
        f.check_all_read("genesis entity");
    }

    void put_plain_account(const EntityDecl& d, const FieldVals& f, Loc loc) {
        Pubkey addr = to_pubkey(eval(*d.target), d.target->loc, "account address");
        Bytes data = f.get("data") ? Codec::to_bytes(*f.get("data"), f.loc("data", loc), "field 'data'") : Bytes{};
        Pubkey owner = f.get("owner") ? to_pubkey(*f.get("owner"), f.loc("owner", loc), "owner") : sol::ids::system_program;
        bool exe = f.get("executable") ? want<bool>(*f.get("executable"), f.loc("executable", loc), "a bool", "executable") : false;
        put(addr, opt_u64(f, "lamports", 0, loc), owner, std::move(data), exe, opt_u64(f, "rent_epoch", 0, loc), d.target->loc, "account");
    }

    void put_program(const EntityDecl& d, const FieldVals& f, Loc loc) {
        backend::ProgramSpec spec;
        spec.name = d.label.name;
        spec.origin = loc;
        spec.id = to_pubkey(f.require("id"), f.loc("id", loc), "program id");
        spec.binary = resolve_path(to_str(f.require("binary"), f.loc("binary", loc), "program binary"));
        if (!std::filesystem::exists(spec.binary))
            fail(f.loc("binary", loc), "program binary '" + spec.binary.string() + "' does not exist");
        spec.loader = f.get("loader") ? to_pubkey(*f.get("loader"), f.loc("loader", loc), "loader") : sol::ids::bpf_upgradeable_loader;
        if (spec.loader != sol::ids::bpf_loader && spec.loader != sol::ids::bpf_loader_deprecated &&
            spec.loader != sol::ids::bpf_upgradeable_loader && spec.loader != sol::ids::loader_v4)
            fail(f.loc("loader", loc), "unknown loader " + spec.loader.base58() +
                                           "; expected bpf_loader(), bpf_loader_deprecated(), bpf_upgradeable_loader() or loader_v4()");
        spec.upgrade_authority = opt_authority(f, "upgrade_authority", loc);
        if (spec.upgrade_authority && spec.loader != sol::ids::bpf_upgradeable_loader && spec.loader != sol::ids::loader_v4)
            fail(f.loc("upgrade_authority", loc), "upgrade_authority requires an upgradeable loader (bpf_upgradeable_loader() or loader_v4())");
        if (const Value* idl = f.get("idl")) {
            spec.idl = resolve_path(to_str(*idl, f.loc("idl", loc), "idl"));
            if (!std::filesystem::exists(*spec.idl)) fail(f.loc("idl", loc), "IDL file '" + spec.idl->string() + "' does not exist");
            load_idl(*spec.idl, f.loc("idl", loc));
            ledger.program_idls[spec.id] = *spec.idl;
        }
        claim(spec.id, d.label.loc, "program '" + spec.name + "'");
        genesis->put_program(spec);
    }

    void put_mint(const EntityDecl& d, const FieldVals& f, Loc loc) {
        Pubkey addr = to_pubkey(eval(*d.target), d.target->loc, "mint address");
        Pubkey tp = token_program_of(f, loc);
        sol::MintLayout m;
        m.decimals = to_u8(f.require("decimals"), f.loc("decimals", loc), "decimals");
        m.mint_authority = opt_authority(f, "mint_authority", loc);
        m.freeze_authority = opt_authority(f, "freeze_authority", loc);
        m.supply = opt_u64(f, "supply", 0, loc);
        m.extensions = extensions_of(f, tp);
        Bytes data = sol::encode_mint(m);
        std::uint64_t lamports = opt_u64(f, "lamports", sol::rent_exempt_minimum(data.size()), loc);
        ledger.mints[addr] = {tp, m.supply, d.target->loc};
        put(addr, lamports, tp, std::move(data), false, 0, d.target->loc, "mint");
    }

    // Plain token accounts and associated token accounts (whose address must
    // be the derived one, and which take no delegate/state/extension fields).
    void put_token_account(const EntityDecl& d, const FieldVals& f, Loc loc) {
        bool ata = d.kind == EntityKind::associated_token_account;
        Pubkey addr = to_pubkey(eval(*d.target), d.target->loc, "token account address");
        Pubkey tp = token_program_of(f, loc);
        sol::TokenAccountLayout a;
        a.mint = to_pubkey(f.require("mint"), f.loc("mint", loc), "mint");
        a.owner = to_pubkey(f.require("owner"), f.loc("owner", loc), "owner");
        a.amount = opt_u64(f, "amount", 0, loc);
        if (ata) {
            Pubkey expected = sol::associated_token_address(a.owner, a.mint, tp);
            if (expected != addr)
                fail(d.target->loc, "address " + addr.base58() + " is not the associated token account of owner " +
                                        a.owner.base58() + " for mint " + a.mint.base58() + " (expected " + expected.base58() + ")");
            if (tp == sol::ids::token_2022_program) a.extensions.push_back({sol::ExtensionType::immutable_owner, {}});
        } else {
            a.delegate = opt_authority(f, "delegate", loc);
            a.delegated_amount = opt_u64(f, "delegated_amount", 0, loc);
            if (a.delegated_amount > 0 && !a.delegate)
                fail(f.loc("delegated_amount", loc), "delegated_amount is set but the account has no delegate");
            if (a.delegated_amount > a.amount)
                fail(f.loc("delegated_amount", loc), "delegated_amount (" + std::to_string(a.delegated_amount) +
                                                         ") exceeds the account amount (" + std::to_string(a.amount) + ")");
            if (const Value* st = f.get("state")) a.state = token_state(*st, f.loc("state", loc));
            a.close_authority = opt_authority(f, "close_authority", loc);
            auto exts = extensions_of(f, tp);
            a.extensions.insert(a.extensions.end(), exts.begin(), exts.end());
        }
        Bytes data = sol::encode_token_account(a);
        std::uint64_t lamports = opt_u64(f, "lamports", sol::rent_exempt_minimum(data.size()), loc);
        ledger.token_accounts.push_back({addr, a.mint, tp, a.amount, d.target->loc});
        put(addr, lamports, tp, std::move(data), false, 0, d.target->loc, ata ? "associated token account" : "token account");
    }

    void put_lookup_table(const EntityDecl& d, const FieldVals& f, Loc loc) {
        Pubkey addr = to_pubkey(eval(*d.target), d.target->loc, "lookup table address");
        sol::LookupTableLayout t;
        t.authority = opt_authority(f, "authority", loc);
        t.deactivation_slot = opt_u64(f, "deactivation_slot", UINT64_MAX, loc);
        t.last_extended_slot = opt_u64(f, "last_extended_slot", 0, loc);
        if (const Value* addrs = f.get("addresses"))
            for (const auto& a : *want<ListP>(*addrs, f.loc("addresses", loc), "a list", "addresses"))
                t.addresses.push_back(to_pubkey(a, f.loc("addresses", loc), "lookup table address"));
        if (t.addresses.size() > max_lookup_table_addresses)
            fail(f.loc("addresses", loc), "a lookup table holds at most " + std::to_string(max_lookup_table_addresses) + " addresses");
        Bytes data = sol::encode_lookup_table(t);
        std::uint64_t lamports = opt_u64(f, "lamports", sol::rent_exempt_minimum(data.size()), loc);
        put(addr, lamports, sol::ids::address_lookup_table_program, std::move(data), false, 0, d.target->loc, "lookup table");
    }

    void exec_sysvar(const Stmt& s, const SysvarDecl& d) {
        backend::SysvarSpec spec{d.name.name, {}, {}, s.loc};
        for (const auto& e : d.body.entries) {
            Value v = eval(*e.value);
            if (v.is<__int128>()) spec.fields[e.key] = v.as<__int128>();
            else if (v.is<double>()) spec.fields[e.key] = v.as<double>();
            else spec.fields[e.key] = want<bool>(v, e.value->loc, "a number or bool", "sysvar field");
            spec.field_origins[e.key] = e.loc;
        }
        genesis->set_sysvar(spec);
    }

    void exec_clone(const Stmt& s, const CloneDecl& c) {
        backend::CloneSpec spec;
        spec.address = to_pubkey(eval(*c.target), c.target->loc, "clone target");
        spec.program = c.program;
        spec.url = source_url(eval(*c.source));
        spec.origin = s.loc;
        if (c.at_slot) spec.slot = to_u64(eval(*c.at_slot), c.at_slot->loc, "clone slot");
        if (c.overrides) {
            FieldVals f = eval_fields(c.overrides->entries);
            if (f.get("lamports")) spec.overrides.lamports = opt_u64(f, "lamports", 0, s.loc);
            if (f.get("rent_epoch")) spec.overrides.rent_epoch = opt_u64(f, "rent_epoch", 0, s.loc);
            if (const Value* o = f.get("owner")) spec.overrides.owner = to_pubkey(*o, f.loc("owner", s.loc), "owner");
            if (const Value* dv = f.get("data")) spec.overrides.data = Codec::to_bytes(*dv, f.loc("data", s.loc), "field 'data'");
            if (const Value* e = f.get("executable")) spec.overrides.executable = want<bool>(*e, f.loc("executable", s.loc), "a bool", "executable");
            f.check_all_read("clone overrides");
        }
        claim(spec.address, c.target->loc, c.program ? "cloned program" : "cloned account");
        genesis->clone_account(spec);
    }

    void exec_delete(const Stmt& s, const DeleteDecl& d) {
        Pubkey addr = to_pubkey(eval(*d.target), d.target->loc, "delete target");
        auto it = ledger.declared.find(addr);
        if (it == ledger.declared.end())
            fail(d.target->loc, "cannot delete " + addr.base58() + ": it is not defined earlier in this genesis");
        ledger.declared.erase(it);
        genesis->remove_account({addr, d.program, s.loc});
        ledger.mints.erase(addr);
        std::erase_if(ledger.token_accounts, [&](const GenesisLedger::TokenInfo& t) { return t.address == addr; });
    }

    // Cross-declaration checks once genesis is complete.
    void validate_genesis() {
        std::map<Pubkey, unsigned __int128> totals;
        for (const auto& t : ledger.token_accounts) {
            auto m = ledger.mints.find(t.mint);
            if (m == ledger.mints.end()) {
                if (!ledger.declared.count(t.mint))
                    diags.warning(t.loc, "token account " + t.address.base58() + " refers to mint " + t.mint.base58() +
                                             ", which is not defined in genesis");
                continue;
            }
            if (m->second.token_program != t.token_program) {
                TideError err(t.loc, "token account " + t.address.base58() + " uses a different token program than its mint");
                err.notes.push_back({m->second.loc, "mint is defined here"});
                throw err;
            }
            totals[t.mint] += t.amount;
        }
        for (const auto& [addr, m] : ledger.mints) {
            unsigned __int128 total = totals.count(addr) ? totals[addr] : 0;
            if (total != m.supply)
                diags.warning(m.loc, "mint " + addr.base58() + " has supply " + std::to_string(m.supply) +
                                         " but its token accounts in genesis hold " + int_to_string(static_cast<__int128>(total)));
        }
    }

    // --------------------------------------------------------- runtime --

    std::vector<sol::Keypair> keypairs_of(const Value& v, Loc loc) {
        std::vector<sol::Keypair> out;
        for (const auto& k : *want<ListP>(v, loc, "a list", "signers")) out.push_back(want<sol::Keypair>(k, loc, "a keypair", "signer"));
        return out;
    }

    sol::AccountMeta meta_of(const Value& v, const std::vector<sol::Keypair>& signers, const Pubkey& fee_payer, Loc loc) {
        auto is_signer = [&](const Pubkey& p) {
            if (p == fee_payer) return true;
            for (const auto& s : signers)
                if (s.pubkey == p) return true;
            return false;
        };
        if (v.is<MetaV>()) {
            const auto& m = v.as<MetaV>();
            return {m.key, m.signer || is_signer(m.key), m.writable};
        }
        const Pubkey& p = to_pubkey(v, loc, "instruction account");
        return {p, is_signer(p), true};
    }

    // Shared by `transaction` and `invoke`: the explicit fee payer, else the first signer.
    Pubkey resolve_fee_payer(const std::optional<Pubkey>& explicit_payer, const std::vector<sol::Keypair>& signers,
                             Loc loc, const char* statement) {
        if (explicit_payer) return *explicit_payer;
        if (signers.empty()) fail(loc, std::string(statement) + " needs 'fee_payer' or at least one entry in 'signers'");
        return signers.front().pubkey;
    }

    backend::TxResult send(backend::TransactionRequest req, Loc loc) {
        bool payer_signs = false;
        for (const auto& s : req.signers) payer_signs |= s.pubkey == req.fee_payer;
        if (!payer_signs) fail(loc, "fee payer " + req.fee_payer.base58() + " has no matching keypair in 'signers'");
        return runtime->send_transaction(req);
    }

    static std::string failure_text(const backend::TxResult& r) {
        std::string msg = r.error;
        if (!r.logs.empty()) {
            msg += "\nprogram logs:";
            for (const auto& l : r.logs) msg += "\n  " + l;
        }
        return msg;
    }

    void exec_transaction(const Stmt& s, const TransactionStmt& t) {
        backend::TransactionRequest req;
        req.origin = s.loc;
        std::optional<Pubkey> fee_payer;
        for (const auto& e : t.entries) {
            if (e.is_instruction) continue;
            Value v = eval(*e.value);
            if (e.key == "fee_payer") fee_payer = to_pubkey(v, e.value->loc, "fee_payer");
            else if (e.key == "signers") req.signers = keypairs_of(v, e.value->loc);
            else if (e.key == "compute_unit_limit") req.compute_unit_limit = to_u32(v, e.value->loc, "compute_unit_limit");
            else if (e.key == "compute_unit_price") req.compute_unit_price = to_u64(v, e.value->loc, "compute_unit_price");
            else unhandled_key("transaction", e.key);
        }
        req.fee_payer = resolve_fee_payer(fee_payer, req.signers, s.loc, "transaction");
        for (const auto& e : t.entries) {
            if (!e.is_instruction) continue;
            sol::Instruction ix;
            ix.program_id = to_pubkey(eval(*e.program), e.program->loc, "instruction program id");
            for (const auto& entry : e.body.entries) {
                Value v = eval(*entry.value);
                if (entry.key == "accounts")
                    for (const auto& a : *want<ListP>(v, entry.value->loc, "a list", "accounts"))
                        ix.accounts.push_back(meta_of(a, req.signers, req.fee_payer, entry.value->loc));
                else if (entry.key == "data") ix.data = Codec::to_bytes(v, entry.value->loc, "instruction data");
                else unhandled_key("instruction", entry.key);
            }
            req.instructions.push_back(std::move(ix));
        }
        backend::TxResult r = send(std::move(req), s.loc);
        if (t.sym) {
            std::vector<Value> logs;
            for (const auto& l : r.logs) logs.emplace_back(l);
            bind(t.sym, make_record({{"success", Value(r.success)},
                                     {"signature", Value(r.signature)},
                                     {"error", Value(r.error)},
                                     {"logs", make_list(std::move(logs))},
                                     {"slot", Value(static_cast<__int128>(r.slot))}}));
            out << "    transaction " << (r.success ? "succeeded" : "failed") << (r.signature.empty() ? "" : ": " + r.signature) << "\n";
        } else if (!r.success) {
            fail(s.loc, "transaction failed: " + failure_text(r));
        } else {
            out << "    transaction succeeded: " << r.signature << "\n";
        }
    }

    void exec_invoke(const Stmt& s, const InvokeStmt& inv) {
        Pubkey program = to_pubkey(eval(*inv.program), inv.program->loc, "invoked program");
        FieldVals f = eval_fields(inv.with.entries);
        Loc loc = s.loc;
        std::string method = to_str(f.require("method"), f.loc("method", loc), "method");
        std::filesystem::path idl_path;
        if (const Value* p = f.get("idl")) idl_path = resolve_path(to_str(*p, f.loc("idl", loc), "idl"));
        else if (auto it = ledger.program_idls.find(program); it != ledger.program_idls.end()) idl_path = it->second;
        else fail(inv.program->loc, "no IDL known for program " + program.base58() +
                                        "; add 'idl = \"...\";' to its genesis program declaration or to this invoke");
        const Json& idl = load_idl(idl_path, loc);

        const Json* ix = nullptr;
        if (idl.contains("instructions"))  // iterate the IDL itself: value() would return a copy
            for (const auto& candidate : idl["instructions"])
                if (same_name(candidate.value("name", ""), method)) ix = &candidate;
        if (!ix) fail(f.loc("method", loc), "IDL '" + idl_path.filename().string() + "' has no instruction '" + method + "'");

        sol::BorshWriter w;
        if (ix->contains("discriminator")) {
            write_discriminator(w, (*ix)["discriminator"], loc);
        } else {
            w.raw(sol::anchor_discriminator("global", snake_case(method)));
        }
        Value args = f.get("args") ? *f.get("args") : make_record({});
        const auto& arg_rec = *want<RecordP>(args, f.loc("args", loc), "an object", "args");
        std::vector<std::string> idl_args;
        for (const auto& a : ix->value("args", Json::array())) {
            std::string an = a.value("name", "");
            idl_args.push_back(an);
            const Value* v = find_field(arg_rec, an);
            if (!v) {
                const std::string ctx = "argument '" + an + "'";
                const Json& at = json_member(a, "type", f.loc("args", loc), ctx);
                if (at.is_object() && at.contains("option")) {
                    w.u8(0);  // omitted optional argument: None
                    continue;
                }
                fail(f.loc("args", loc), "missing argument '" + an + "' for instruction '" + method + "'");
            }
            codec.encode(w, *codec.from_idl(idl, json_member(a, "type", loc, "argument '" + an + "'"), f.loc("args", loc)), *v, f.loc("args", loc), "argument '" + an + "'");
        }
        if (const std::string* n = first_unknown_field(arg_rec, idl_args))
            fail(f.loc("args", loc), "instruction '" + method + "' has no argument '" + *n + "'");

        std::vector<sol::Keypair> signers = f.get("signers") ? keypairs_of(*f.get("signers"), f.loc("signers", loc)) : std::vector<sol::Keypair>{};
        std::optional<Pubkey> explicit_payer;
        if (const Value* fp = f.get("fee_payer")) explicit_payer = to_pubkey(*fp, f.loc("fee_payer", loc), "fee_payer");
        Pubkey fee_payer = resolve_fee_payer(explicit_payer, signers, loc, "invoke");

        Value accounts_v = f.get("accounts") ? *f.get("accounts") : make_record({});
        const auto& acc_rec = *want<RecordP>(accounts_v, f.loc("accounts", loc), "an object", "accounts");
        sol::Instruction instr;
        instr.program_id = program;
        instr.data = w.take();
        std::vector<std::string> used;
        std::function<void(const Json&)> add_accounts = [&](const Json& list) {
            for (const auto& a : list) {
                if (a.contains("accounts")) {
                    add_accounts(a["accounts"]);
                    continue;
                }
                std::string an = a.value("name", "");
                bool writable = a.value("writable", a.value("isMut", false));
                bool is_signer = a.value("signer", a.value("isSigner", false));
                bool optional = a.value("optional", a.value("isOptional", false));
                const Value* v = find_field(acc_rec, an);
                Pubkey key;
                if (v) {
                    used.push_back(an);
                    key = v->is<MetaV>() ? v->as<MetaV>().key : to_pubkey(*v, f.loc("accounts", loc), "account '" + an + "'");
                } else if (a.contains("address")) {
                    key = sol::pubkey_const(a["address"].get<std::string>());
                } else if (snake_case(an) == "system_program") {
                    key = sol::ids::system_program;
                } else if (snake_case(an) == "token_program") {
                    key = sol::ids::token_program;
                } else if (snake_case(an) == "associated_token_program") {
                    key = sol::ids::associated_token_program;
                } else if (snake_case(an) == "rent") {
                    key = sol::ids::sysvar_rent;
                } else if (optional) {
                    key = program;  // Anchor encodes a missing optional account as the program id
                    writable = false;
                } else {
                    fail(f.loc("accounts", loc), "missing account '" + an + "' for instruction '" + method + "'");
                }
                instr.accounts.push_back({key, is_signer, writable});
            }
        };
        add_accounts(ix->value("accounts", Json::array()));
        if (const std::string* n = first_unknown_field(acc_rec, used))
            fail(f.loc("accounts", loc), "instruction '" + method + "' has no account named '" + *n + "'");
        if (const Value* rem = f.get("remaining_accounts"))
            for (const auto& a : *want<ListP>(*rem, f.loc("remaining_accounts", loc), "a list", "remaining_accounts"))
                instr.accounts.push_back(meta_of(a, signers, fee_payer, f.loc("remaining_accounts", loc)));

        backend::TransactionRequest req;
        req.origin = loc;
        req.fee_payer = fee_payer;
        req.signers = std::move(signers);
        req.instructions.push_back(std::move(instr));
        if (const Value* cu = f.get("compute_unit_limit"))
            req.compute_unit_limit = to_u32(*cu, f.loc("compute_unit_limit", loc), "compute_unit_limit");
        f.check_all_read("invoke");
        auto r = send(std::move(req), loc);
        if (!r.success) fail(loc, "invoke of '" + method + "' failed: " + failure_text(r));
        out << "    invoked " << method << ": " << r.signature << "\n";
    }

    // ------------------------------------------------------- lifecycle --

    backend::NetworkConfig build_config(const NetworkDecl& net) {
        backend::NetworkConfig cfg;
        cfg.name = net.name.name;
        cfg.origin = net.name.loc;
        cfg.base_dir = base_dir();
        for (const auto& e : net.entries) {
            if (const auto* o = std::get_if<OptionsEntry>(&e.node)) {
                for (const auto& entry : o->body.entries) {
                    Value v = eval(*entry.value);
                    Loc l = entry.value->loc;
                    const std::string& k = entry.key;
                    if (k == "slots_per_epoch") cfg.slots_per_epoch = to_u64(v, l, k);
                    else if (k == "ticks_per_slot") cfg.ticks_per_slot = to_u64(v, l, k);
                    else if (k == "compute_unit_limit") cfg.compute_unit_limit = to_u64(v, l, k);
                    else if (k == "limit_ledger_size") cfg.limit_ledger_size = to_u64(v, l, k);
                    else if (k == "warp_slot") cfg.warp_slot = to_u64(v, l, k);
                    else if (k == "faucet_sol") cfg.faucet_sol = to_double(v, l, k);
                    else if (k == "ledger") cfg.ledger = to_str(v, l, k);
                    else if (k == "bind_address") cfg.bind_address = to_str(v, l, k);
                    else if (k == "startup_timeout") cfg.startup_timeout_ms = want<Duration>(v, l, "a duration", k).ms;
                    else if (k == "confirm_timeout") cfg.confirm_timeout_ms = want<Duration>(v, l, "a duration", k).ms;
                    else unhandled_key("options", k);
                }
            } else if (const auto* vd = std::get_if<ValidatorEntry>(&e.node)) {
                backend::ValidatorConfig vc;
                vc.name = vd->name.name;
                vc.origin = vd->name.loc;
                for (const auto& entry : vd->body.entries) {
                    Value v = eval(*entry.value);
                    Loc l = entry.value->loc;
                    const std::string& k = entry.key;
                    auto port = [&] {
                        __int128 p = to_int(v, l, k);
                        if (p < 1 || p > 65535) fail(l, k + " must be between 1 and 65535");
                        return static_cast<std::uint16_t>(p);
                    };
                    if (k == "rpc_port") vc.rpc_port = port();
                    else if (k == "gossip_port") vc.gossip_port = port();
                    else if (k == "faucet_port") vc.faucet_port = port();
                    else if (k == "dynamic_port_range") vc.dynamic_port_range = to_str(v, l, k);
                    else if (k == "bind_address") vc.bind_address = to_str(v, l, k);
                    else if (k == "identity") vc.identity = want<sol::Keypair>(v, l, "a keypair", k);
                    else unhandled_key("validator", k);
                }
                cfg.validators.push_back(std::move(vc));
            }
        }
        if (cfg.confirm_timeout_ms) confirm_timeout_ms = *cfg.confirm_timeout_ms;
        return cfg;
    }

    // An exception that escaped execution, as a diagnostic. A TideError carries
    // its own location and notes; anything else is reported at `fallback`.
    void report(const std::exception& e, const std::string& prefix = {}, Loc fallback = {}) {
        if (const auto* te = dynamic_cast<const TideError*>(&e)) {
            auto& d = diags.error(te->loc, prefix + te->what());
            for (const auto& n : te->notes) d.notes.push_back(n);
        } else {
            diags.error(fallback, prefix + e.what());
        }
    }

    RunResult run(const NetworkDecl& net, backend::NetworkBackend& be, const RunOptions& opts) {
        RunResult res;
        ledger = {};
        Frame net_frame;
        Restore in_network(frame, &net_frame);
        try {
            // Network-level bindings.
            for (const auto& e : net.entries)
                if (const auto* l = std::get_if<LetStmt>(&e.node)) exec_binding(*l);

            // Configuration.
            out << "==> configuring network '" << net.name.name << "'\n";
            be.configure(build_config(net));

            // Genesis.
            {
                Restore building(genesis, &be.genesis());
                for (const auto& e : net.entries)
                    if (const auto* g = std::get_if<GenesisEntry>(&e.node)) {
                        out << "==> building genesis state\n";
                        scoped([&] { return exec_block(g->body); });
                    }
                validate_genesis();
            }
            out << "    " << ledger.declared.size() << " genesis account(s) defined\n";

            // Bootstrap on a private instance.
            const BootstrapEntry* bootstrap = nullptr;
            for (const auto& e : net.entries)
                if (const auto* b = std::get_if<BootstrapEntry>(&e.node)) bootstrap = b;
            if (opts.dry_run) {
                be.persist();
                out << "==> dry run: state persisted; bootstrap, launch and scenarios skipped\n";
                res.ok = true;
                return res;
            }
            if (bootstrap) {
                out << "==> running bootstrap\n";
                Restore bootstrapping(runtime, &be.start_bootstrap());
                scoped([&] { return exec_block(bootstrap->body); });
            }

            out << "==> persisting state\n";
            be.persist();
            out << "==> launching network\n";
            backend::RuntimeClient& client = be.launch();

            // Scenarios run against the launched network; failures are reported
            // but do not take the network down.
            if (opts.run_scenarios) {
                for (const auto& e : net.entries) {
                    const auto* sc = std::get_if<ScenarioEntry>(&e.node);
                    if (!sc) continue;
                    if (!opts.only_scenarios.empty() &&
                        std::find(opts.only_scenarios.begin(), opts.only_scenarios.end(), sc->name.name) == opts.only_scenarios.end())
                        continue;
                    ++res.scenarios_run;
                    out << "==> scenario '" << sc->name.name << "'\n";
                    Restore live(runtime, &client);
                    try {
                        scoped([&] { return exec_block(sc->body); });
                        out << "    scenario '" << sc->name.name << "' passed\n";
                    } catch (const std::exception& err) {  // never take the launched network down
                        ++res.scenarios_failed;
                        report(err, "scenario '" + sc->name.name + "' failed: ", sc->name.loc);
                        out << "    scenario '" << sc->name.name << "' FAILED\n";
                    }
                }
            }
            res.launch = be.detach();
            res.ok = true;
        } catch (const std::exception& err) {
            report(err);
            be.abort();
        }
        return res;
    }
};

Interpreter::Interpreter(const sema::Program& program, Diagnostics& diags, std::ostream& out)
    : impl_(std::make_unique<Impl>(program, diags, out)) {}
Interpreter::~Interpreter() = default;

bool Interpreter::init_globals() {
    try {
        for (const sema::Module* m : impl_->prog.init_order)
            for (const auto& d : m->doc->decls)
                if (const auto* l = std::get_if<LetStmt>(&d.node)) impl_->exec_binding(*l);
        return true;
    } catch (const std::exception& e) {
        impl_->report(e);
    }
    return false;
}

RunResult Interpreter::run(const NetworkDecl& network, backend::NetworkBackend& backend, const RunOptions& options) {
    return impl_->run(network, backend, options);
}

}  // namespace tide
