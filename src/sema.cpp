#include "sema.hpp"

#include <functional>
#include <set>
#include <span>

#include "parser.hpp"
#include "solana.hpp"

namespace tide::sema {

using namespace ast;

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

// ============================================================ TypeContext ==

TypeContext::TypeContext() {
    auto prim = [&](TypeKind k) { return make(Type{k}); };
    error_ = prim(TypeKind::error);
    any_ = prim(TypeKind::any);
    void_t_ = prim(TypeKind::void_);
    null_ = prim(TypeKind::null);
    bool_t_ = prim(TypeKind::bool_);
    float_t_ = prim(TypeKind::float_);
    {
        Type f{TypeKind::float_};
        f.width = IntWidth::u32;
        float32_ = make(std::move(f));
    }
    string_ = prim(TypeKind::string);
    bytes_ = prim(TypeKind::bytes);
    duration_ = prim(TypeKind::duration);
    pubkey_ = prim(TypeKind::pubkey);
    keypair_ = prim(TypeKind::keypair);
    source_ = prim(TypeKind::source);
    range_ = prim(TypeKind::range);
    meta_ = prim(TypeKind::account_meta);
    for (int i = 0; i < 11; ++i) {
        Type t{TypeKind::int_};
        t.width = static_cast<IntWidth>(i);
        ints_[i] = make(std::move(t));
    }
    account_info_ = record({{"address", pubkey_}, {"lamports", int_(IntWidth::u64)}, {"owner", pubkey_},
                            {"data", bytes_}, {"executable", bool_t_}, {"rent_epoch", int_(IntWidth::u64)}});
    tx_result_ = record({{"success", bool_t_}, {"signature", string_}, {"error", string_},
                         {"logs", list(string_)}, {"slot", int_(IntWidth::u64)}});
}

const Type* TypeContext::make(Type t) {
    arena_.push_back(std::move(t));
    return &arena_.back();
}
const Type* TypeContext::list(const Type* elem, std::optional<std::uint64_t> len) {
    Type t{TypeKind::list};
    t.elem = elem;
    t.fixed_len = len;
    return make(std::move(t));
}
const Type* TypeContext::option(const Type* inner) {
    if (inner->kind == TypeKind::option || inner->kind == TypeKind::error) return inner;
    Type t{TypeKind::option};
    t.elem = inner;
    return make(std::move(t));
}
const Type* TypeContext::record(std::vector<std::pair<std::string, const Type*>> fields, const Symbol* schema) {
    Type t{TypeKind::record};
    t.fields = std::move(fields);
    t.schema = schema;
    return make(std::move(t));
}
const Type* TypeContext::tuple(std::vector<const Type*> elems) {
    Type t{TypeKind::tuple};
    t.elems = std::move(elems);
    return make(std::move(t));
}
const Type* TypeContext::schema(const Symbol* s) {
    Type t{TypeKind::schema};
    t.schema = s;
    return make(std::move(t));
}
const Type* TypeContext::module(const Module* m) {
    Type t{TypeKind::module};
    t.module = m;
    return make(std::move(t));
}

std::string to_string(IntWidth w) {
    static const char* names[] = {"int", "u8", "u16", "u32", "u64", "u128", "i8", "i16", "i32", "i64", "i128"};
    return names[static_cast<int>(w)];
}

std::pair<__int128, __int128> int_bounds(IntWidth w) {
    constexpr __int128 i128_max = static_cast<__int128>((~static_cast<unsigned __int128>(0)) >> 1);
    constexpr __int128 i128_min = -i128_max - 1;
    switch (w) {
        case IntWidth::u8: return {0, 0xff};
        case IntWidth::u16: return {0, 0xffff};
        case IntWidth::u32: return {0, 0xffffffffLL};
        case IntWidth::u64: return {0, static_cast<__int128>(UINT64_MAX)};
        case IntWidth::u128: return {0, i128_max};  // values are 128-bit signed at runtime
        case IntWidth::i8: return {INT8_MIN, INT8_MAX};
        case IntWidth::i16: return {INT16_MIN, INT16_MAX};
        case IntWidth::i32: return {INT32_MIN, INT32_MAX};
        case IntWidth::i64: return {INT64_MIN, INT64_MAX};
        case IntWidth::i128:
        case IntWidth::any: return {i128_min, i128_max};
    }
    return {i128_min, i128_max};
}

std::string to_string(const Type* t) {
    if (!t) return "<none>";
    switch (t->kind) {
        case TypeKind::error: return "<error>";
        case TypeKind::any: return "any";
        case TypeKind::void_: return "void";
        case TypeKind::null: return "null";
        case TypeKind::bool_: return "bool";
        case TypeKind::int_: return to_string(t->width);
        case TypeKind::float_: return t->width == IntWidth::u32 ? "f32" : "float";
        case TypeKind::string: return "string";
        case TypeKind::bytes: return "bytes";
        case TypeKind::duration: return "duration";
        case TypeKind::pubkey: return "pubkey";
        case TypeKind::keypair: return "keypair";
        case TypeKind::source: return "source";
        case TypeKind::range: return "range";
        case TypeKind::account_meta: return "account_meta";
        case TypeKind::list:
            return t->fixed_len ? "[" + to_string(t->elem) + "; " + std::to_string(*t->fixed_len) + "]"
                                : "[" + to_string(t->elem) + "]";
        case TypeKind::option: return "Option<" + to_string(t->elem) + ">";
        case TypeKind::record: {
            if (t->schema) return t->schema->name;
            std::string s = "{";
            for (std::size_t i = 0; i < t->fields.size(); ++i)
                s += (i ? ", " : "") + t->fields[i].first + ": " + to_string(t->fields[i].second);
            return s + "}";
        }
        case TypeKind::tuple: {
            std::string s = "(";
            for (std::size_t i = 0; i < t->elems.size(); ++i) s += (i ? ", " : "") + to_string(t->elems[i]);
            return s + ")";
        }
        case TypeKind::schema: return "schema " + t->schema->name;
        case TypeKind::module: return "module";
    }
    return "?";
}

const ast::NetworkDecl* Program::find_network(std::string_view name) const {
    for (const auto* n : networks())
        if (n->name.name == name) return n;
    return nullptr;
}

std::vector<const ast::NetworkDecl*> Program::networks() const {
    std::vector<const ast::NetworkDecl*> out;
    for (const auto& d : root()->doc->decls)
        if (const auto* n = std::get_if<NetworkDecl>(&d.node)) out.push_back(n);
    return out;
}

// =============================================================== builtins ==

namespace {

enum class Effect { pure, state, runtime_only };

struct BuiltinDef {
    std::string_view name;
    BuiltinId id;
    Effect effect;
};

constexpr BuiltinDef builtin_table[] = {
    {"seed", BuiltinId::seed, Effect::pure},
    {"keypair_file", BuiltinId::keypair_file, Effect::pure},
    {"pubkey", BuiltinId::pubkey, Effect::pure},
    {"pda", BuiltinId::pda, Effect::pure},
    {"pda_bump", BuiltinId::pda_bump, Effect::pure},
    {"ata", BuiltinId::ata, Effect::pure},
    {"system_program", BuiltinId::system_program, Effect::pure},
    {"token_program", BuiltinId::token_program, Effect::pure},
    {"token_2022_program", BuiltinId::token_2022_program, Effect::pure},
    {"associated_token_program", BuiltinId::associated_token_program, Effect::pure},
    {"bpf_loader", BuiltinId::bpf_loader, Effect::pure},
    {"bpf_loader_deprecated", BuiltinId::bpf_loader_deprecated, Effect::pure},
    {"bpf_upgradeable_loader", BuiltinId::bpf_upgradeable_loader, Effect::pure},
    {"loader_v4", BuiltinId::loader_v4, Effect::pure},
    {"memo_program", BuiltinId::memo_program, Effect::pure},
    {"compute_budget_program", BuiltinId::compute_budget_program, Effect::pure},
    {"address_lookup_table_program", BuiltinId::address_lookup_table_program, Effect::pure},
    {"hex", BuiltinId::hex, Effect::pure},
    {"base64", BuiltinId::base64, Effect::pure},
    {"base58", BuiltinId::base58, Effect::pure},
    {"utf8", BuiltinId::utf8, Effect::pure},
    {"sha256", BuiltinId::sha256, Effect::pure},
    {"file", BuiltinId::file, Effect::pure},
    {"u8", BuiltinId::u8, Effect::pure},
    {"u16_le", BuiltinId::u16_le, Effect::pure},
    {"u32_le", BuiltinId::u32_le, Effect::pure},
    {"u64_le", BuiltinId::u64_le, Effect::pure},
    {"i64_le", BuiltinId::i64_le, Effect::pure},
    {"borsh", BuiltinId::borsh, Effect::pure},
    {"anchor", BuiltinId::anchor, Effect::pure},
    {"decode", BuiltinId::decode, Effect::pure},
    {"len", BuiltinId::len, Effect::pure},
    {"str", BuiltinId::str, Effect::pure},
    {"print", BuiltinId::print, Effect::pure},
    {"rent_exempt", BuiltinId::rent_exempt, Effect::pure},
    {"rpc", BuiltinId::rpc, Effect::pure},
    {"readonly", BuiltinId::readonly, Effect::pure},
    {"writable", BuiltinId::writable, Effect::pure},
    {"signer", BuiltinId::signer, Effect::pure},
    {"sol", BuiltinId::sol, Effect::pure},
    {"system_transfer", BuiltinId::system_transfer, Effect::pure},
    {"spl_transfer", BuiltinId::spl_transfer, Effect::pure},
    {"spl_transfer_checked", BuiltinId::spl_transfer_checked, Effect::pure},
    {"spl_mint_to", BuiltinId::spl_mint_to, Effect::pure},
    {"memo", BuiltinId::memo, Effect::pure},
    {"balance", BuiltinId::balance, Effect::state},
    {"token_balance", BuiltinId::token_balance, Effect::state},
    {"account_info", BuiltinId::account, Effect::state},
    {"exists", BuiltinId::exists, Effect::state},
    {"slot", BuiltinId::slot, Effect::runtime_only},
    {"epoch", BuiltinId::epoch, Effect::runtime_only},
};

const BuiltinDef* find_builtin(std::string_view name) {
    for (const auto& b : builtin_table)
        if (b.name == name) return &b;
    return nullptr;
}

}  // namespace

const char* builtin_name(BuiltinId id) {
    for (const auto& b : builtin_table)
        if (b.id == id) return b.name.data();
    return "<builtin>";
}

// ================================================================ checker ==

namespace {

enum class Ctx { top_level, function, config, genesis, runtime };

// Field/key specifications for record-like blocks.
enum class Ty {
    any, int_, u8, u16, u32, u64, float_, number, bool_, string, bytes, pubkey, opt_pubkey, keypair, source,
    duration, list_pubkey, list_keypair, list_meta, record,
};

struct FieldSpec {
    std::string_view name;
    Ty type;
    bool required = false;
};

struct ExtSpec {
    std::string_view name;
    std::span<const FieldSpec> fields;
};

struct BlockSpec {
    std::string_view what;
    std::span<const FieldSpec> fields;
    std::span<const ExtSpec> extensions = {};
};

// --- genesis entities
constexpr FieldSpec account_fields[] = {
    {"lamports", Ty::u64, true}, {"owner", Ty::pubkey}, {"data", Ty::bytes},
    {"executable", Ty::bool_},   {"rent_epoch", Ty::u64},
};
constexpr FieldSpec clone_override_fields[] = {
    {"lamports", Ty::u64}, {"owner", Ty::pubkey}, {"data", Ty::bytes}, {"executable", Ty::bool_}, {"rent_epoch", Ty::u64},
};
constexpr FieldSpec program_fields[] = {
    {"id", Ty::pubkey, true}, {"binary", Ty::string, true}, {"loader", Ty::pubkey},
    {"upgrade_authority", Ty::opt_pubkey}, {"idl", Ty::string},
};
constexpr FieldSpec mint_fields[] = {
    {"token_program", Ty::pubkey},   {"decimals", Ty::u8, true}, {"mint_authority", Ty::opt_pubkey},
    {"freeze_authority", Ty::opt_pubkey}, {"supply", Ty::u64},   {"lamports", Ty::u64},
};
constexpr FieldSpec token_account_fields[] = {
    {"token_program", Ty::pubkey}, {"mint", Ty::pubkey, true}, {"owner", Ty::pubkey, true},
    {"amount", Ty::u64},           {"delegate", Ty::opt_pubkey}, {"delegated_amount", Ty::u64},
    {"state", Ty::string},         {"close_authority", Ty::opt_pubkey}, {"lamports", Ty::u64},
};
constexpr FieldSpec ata_fields[] = {
    {"token_program", Ty::pubkey}, {"mint", Ty::pubkey, true}, {"owner", Ty::pubkey, true},
    {"amount", Ty::u64},           {"lamports", Ty::u64},
};
constexpr FieldSpec lookup_table_fields[] = {
    {"authority", Ty::opt_pubkey},  {"addresses", Ty::list_pubkey}, {"deactivation_slot", Ty::u64},
    {"last_extended_slot", Ty::u64}, {"lamports", Ty::u64},
};

constexpr FieldSpec ext_close_authority[] = {{"close_authority", Ty::opt_pubkey}};
constexpr FieldSpec ext_permanent_delegate[] = {{"delegate", Ty::opt_pubkey}};
constexpr FieldSpec ext_metadata_pointer[] = {{"authority", Ty::opt_pubkey}, {"metadata_address", Ty::opt_pubkey}};
constexpr FieldSpec ext_default_state[] = {{"state", Ty::string, true}};
constexpr ExtSpec mint_extensions[] = {
    {"mint_close_authority", ext_close_authority}, {"permanent_delegate", ext_permanent_delegate},
    {"non_transferable", {}},                       {"metadata_pointer", ext_metadata_pointer},
    {"default_account_state", ext_default_state},
};
constexpr ExtSpec token_account_extensions[] = {{"immutable_owner", {}}};

// --- sysvars
constexpr FieldSpec clock_fields[] = {
    {"slot", Ty::u64}, {"epoch", Ty::u64}, {"unix_timestamp", Ty::int_}, {"epoch_start_timestamp", Ty::int_},
    {"leader_schedule_epoch", Ty::u64},
};
constexpr FieldSpec rent_fields[] = {
    {"lamports_per_byte_year", Ty::u64}, {"exemption_threshold", Ty::number}, {"burn_percent", Ty::u8},
};
constexpr FieldSpec epoch_schedule_fields[] = {
    {"slots_per_epoch", Ty::u64}, {"leader_schedule_slot_offset", Ty::u64}, {"warmup", Ty::bool_},
    {"first_normal_epoch", Ty::u64}, {"first_normal_slot", Ty::u64},
};
struct SysvarSpec {
    std::string_view name;
    std::span<const FieldSpec> fields;
};
constexpr SysvarSpec sysvar_specs[] = {
    {"clock", clock_fields}, {"rent", rent_fields}, {"epoch_schedule", epoch_schedule_fields},
};

// --- network configuration
constexpr FieldSpec option_fields[] = {
    {"slots_per_epoch", Ty::u64},     {"ticks_per_slot", Ty::u64},     {"ledger", Ty::string},
    {"faucet_sol", Ty::number},       {"compute_unit_limit", Ty::u64}, {"limit_ledger_size", Ty::u64},
    {"warp_slot", Ty::u64},           {"bind_address", Ty::string},    {"startup_timeout", Ty::duration},
    {"confirm_timeout", Ty::duration},
};
constexpr FieldSpec validator_fields[] = {
    {"rpc_port", Ty::u16},         {"gossip_port", Ty::u16},  {"faucet_port", Ty::u16},
    {"dynamic_port_range", Ty::string}, {"identity", Ty::keypair}, {"bind_address", Ty::string},
};

// --- runtime
constexpr FieldSpec tx_fields[] = {
    {"fee_payer", Ty::pubkey}, {"signers", Ty::list_keypair}, {"compute_unit_limit", Ty::u32},
    {"compute_unit_price", Ty::u64},
};
constexpr FieldSpec instruction_fields[] = {{"accounts", Ty::list_meta}, {"data", Ty::bytes}};
constexpr FieldSpec invoke_fields[] = {
    {"method", Ty::string, true}, {"accounts", Ty::record}, {"args", Ty::record}, {"signers", Ty::list_keypair},
    {"fee_payer", Ty::pubkey},    {"idl", Ty::string},      {"compute_unit_limit", Ty::u32},
    {"remaining_accounts", Ty::list_meta},
};

struct Scope {
    Scope* parent = nullptr;
    std::map<std::string, Symbol*, std::less<>> names;
};

class Checker {
public:
    Checker(Program& prog, SourceManager& sm, Diagnostics& diags) : prog_(prog), T(prog.types), sm_(sm), diags_(diags) {}

    Module* load_root(Document& doc) {
        auto m = std::make_unique<Module>();
        m->doc = &doc;
        m->dir = doc.file ? doc.file->path.parent_path() : std::filesystem::current_path();
        Module* raw = m.get();
        prog_.modules.push_back(std::move(m));
        if (doc.file) {
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(doc.file->path, ec);
            loaded_[ec ? doc.file->path : canon] = raw;
            in_progress_.insert(ec ? doc.file->path : canon);
        }
        check_module(*raw);
        return raw;
    }

private:
    // ------------------------------------------------------------ scopes --

    struct ScopeGuard {
        Checker& c;
        Scope scope;
        explicit ScopeGuard(Checker& ch) : c(ch) {
            scope.parent = c.scope_;
            c.scope_ = &scope;
        }
        ~ScopeGuard() { c.scope_ = scope.parent; }
    };

    Symbol* new_symbol(SymbolKind kind, const Ident& name, const Type* type) {
        Symbol& s = prog_.symbols.emplace_back();
        s.kind = kind;
        s.name = name.name;
        s.loc = name.loc;
        s.type = type;
        s.owner = module_;
        return &s;
    }

    Symbol* declare(SymbolKind kind, const Ident& name, const Type* type) {
        Symbol* s = new_symbol(kind, name, type);
        auto [it, inserted] = scope_->names.try_emplace(name.name, s);
        if (!inserted) {
            diags_.error(name.loc, "redeclaration of '" + name.name + "'")
                .notes.push_back({it->second->loc, "previous declaration is here"});
        }
        return s;
    }

    Symbol* lookup(std::string_view name) const {
        for (Scope* s = scope_; s; s = s->parent)
            if (auto it = s->names.find(name); it != s->names.end()) return it->second;
        return nullptr;
    }

    std::vector<std::string> visible_names() const {
        std::vector<std::string> out;
        for (Scope* s = scope_; s; s = s->parent)
            for (const auto& [n, _] : s->names) out.push_back(n);
        return out;
    }

    // ----------------------------------------------------------- modules --

    void check_module(Module& m) {
        Module* saved_module = module_;
        Scope* saved_scope = scope_;
        Ctx saved_ctx = ctx_;
        module_ = &m;
        Scope global;
        scope_ = &global;
        ctx_ = Ctx::top_level;

        std::map<std::string, Loc> networks;
        for (auto& d : m.doc->decls) {
            std::visit(overloaded{
                           [&](ImportDecl& i) { check_import(i); },
                           [&](LetStmt& l) {
                               check_binding(l, /*global=*/true);
                           },
                           [&](SchemaDecl& s) { check_schema(s); },
                           [&](FunctionDecl& f) { check_function(f); },
                           [&](NetworkDecl& n) {
                               auto [it, ok] = networks.try_emplace(n.name.name, n.name.loc);
                               if (!ok)
                                   diags_.error(n.name.loc, "duplicate network '" + n.name.name + "'")
                                       .notes.push_back({it->second, "previous definition is here"});
                               check_network(n);
                           },
                       },
                       d.node);
        }
        for (const auto& [name, sym] : global.names) m.exports[name] = sym;
        prog_.init_order.push_back(&m);

        module_ = saved_module;
        scope_ = saved_scope;
        ctx_ = saved_ctx;
    }

    void check_import(ImportDecl& imp) {
        std::filesystem::path p = module_->dir / imp.path;
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(p, ec);
        if (ec) canon = p;
        Module* target = nullptr;
        if (auto it = loaded_.find(canon); it != loaded_.end()) {
            target = it->second;
            if (in_progress_.count(canon)) {
                diags_.error(imp.path_loc, "import cycle: '" + imp.path + "' is already being imported");
                target = nullptr;
            }
        } else {
            const SourceFile* file = sm_.load(canon);
            if (!file) {
                diags_.error(imp.path_loc, "cannot open imported file '" + canon.string() + "'");
            } else if (auto doc = parse_document(*file, diags_)) {
                auto owned = std::make_unique<Document>(std::move(*doc));
                auto m = std::make_unique<Module>();
                m->doc = owned.get();
                m->dir = canon.parent_path();
                target = m.get();
                prog_.imported_docs.push_back(std::move(owned));
                prog_.modules.push_back(std::move(m));
                loaded_[canon] = target;
                in_progress_.insert(canon);
                check_module(*target);
                in_progress_.erase(canon);
            }
        }
        imp.module = target;
        Symbol* s = declare(SymbolKind::module, imp.alias, target ? T.module(target) : T.error());
        s->module = target;
        s->is_global = true;
        imp.sym = s;
    }

    // ---------------------------------------------------------- bindings --

    void check_binding(LetStmt& l, bool global) {
        const Type* type = nullptr;
        switch (l.kind) {
            case BindingKind::let: {
                if (l.annotation) {
                    const Type* ann = resolve_type(*l.annotation, false);
                    expect(*l.init, ann, "initializer of '" + l.name.name + "'");
                    type = ann;
                } else {
                    type = value_type(*l.init, "initializer of '" + l.name.name + "'");
                    if (type->kind == TypeKind::null) {
                        diags_.error(l.init->loc, "cannot infer the type of '" + l.name.name +
                                                      "' from null; add a type annotation such as ': Option<pubkey>'");
                        type = T.error();
                    }
                }
                l.sym = declare(SymbolKind::variable, l.name, type);
                l.sym->is_mutable = true;
                break;
            }
            case BindingKind::keypair:
                expect(*l.init, T.keypair(), "keypair '" + l.name.name + "'");
                l.sym = declare(SymbolKind::keypair, l.name, T.keypair());
                break;
            case BindingKind::source:
                expect_source(*l.init, "source '" + l.name.name + "'");
                l.sym = declare(SymbolKind::source, l.name, T.source());
                break;
        }
        l.sym->is_global = global;
    }

    // ----------------------------------------------------------- schemas --

    void check_schema(SchemaDecl& s) {
        Symbol* sym = declare(SymbolKind::schema, s.name, nullptr);
        sym->schema = &s;
        sym->is_global = true;
        s.sym = sym;
        sym->type = T.schema(sym);
        if (s.format.name != "borsh" && s.format.name != "anchor") {
            std::string_view formats[] = {"borsh", "anchor"};
            diags_.error(s.format.loc, "unknown schema format '" + s.format.name + "'; expected 'borsh' or 'anchor'" +
                                           suggest(s.format.name, formats));
        }
        sym->schema_format = s.format.name;
        std::vector<std::pair<std::string, const Type*>> fields;
        std::map<std::string, Loc> seen;
        for (auto& f : s.fields) {
            auto [it, ok] = seen.try_emplace(f.name.name, f.name.loc);
            if (!ok)
                diags_.error(f.name.loc, "duplicate field '" + f.name.name + "' in schema '" + s.name.name + "'")
                    .notes.push_back({it->second, "previous field is here"});
            fields.emplace_back(f.name.name, resolve_type(f.type, true));
        }
        sym->schema_record = T.record(std::move(fields), sym);
    }

    // --------------------------------------------------------- functions --

    void check_function(FunctionDecl& f) {
        Symbol* sym = declare(SymbolKind::function, f.name, nullptr);
        sym->function = &f;
        sym->is_global = true;
        f.sym = sym;
        for (auto& p : f.params) sym->param_types.push_back(resolve_type(p.type, false));
        sym->return_type = f.ret ? resolve_type(*f.ret, false) : T.void_();

        Ctx saved_ctx = ctx_;
        const Symbol* saved_fn = function_;
        int saved_loops = loop_depth_;
        ctx_ = Ctx::function;
        function_ = sym;
        loop_depth_ = 0;
        {
            ScopeGuard g(*this);
            for (std::size_t i = 0; i < f.params.size(); ++i) {
                f.params[i].sym = declare(SymbolKind::parameter, f.params[i].name, sym->param_types[i]);
            }
            for (auto& st : f.body) stmt(*st);
        }
        if (sym->return_type->kind != TypeKind::void_ && sym->return_type->kind != TypeKind::error &&
            !block_terminates(f.body))
            diags_.error(f.name.loc, "function '" + f.name.name + "' may reach the end of its body without returning a " +
                                         to_string(sym->return_type));
        ctx_ = saved_ctx;
        function_ = saved_fn;
        loop_depth_ = saved_loops;
    }

    static bool block_terminates(const Block& b) {
        if (b.empty()) return false;
        const Stmt& last = *b.back();
        if (last.as<ReturnStmt>()) return true;
        if (const auto* i = last.as<IfStmt>())
            return i->else_block && block_terminates(i->then_block) && block_terminates(*i->else_block);
        return false;
    }

    // ---------------------------------------------------------- networks --

    void check_network(NetworkDecl& n) {
        ScopeGuard g(*this);
        std::optional<Loc> options_loc, genesis_loc, bootstrap_loc;
        std::map<std::string, Loc> validators, scenarios;
        auto once = [&](std::optional<Loc>& slot, Loc at, const char* what) {
            if (slot) {
                diags_.error(at, std::string("network '") + n.name.name + "' already has " + what)
                    .notes.push_back({*slot, "previous definition is here"});
            }
            slot = at;
        };
        for (auto& e : n.entries) {
            std::visit(overloaded{
                           [&](OptionsEntry& o) {
                               once(options_loc, e.loc, "an options block");
                               with_ctx(Ctx::config, [&] { check_block(o.body, {"options", option_fields}); });
                           },
                           [&](ValidatorEntry& v) {
                               auto [it, ok] = validators.try_emplace(v.name.name, v.name.loc);
                               if (!ok)
                                   diags_.error(v.name.loc, "duplicate validator '" + v.name.name + "'")
                                       .notes.push_back({it->second, "previous definition is here"});
                               with_ctx(Ctx::config, [&] { check_block(v.body, {"validator", validator_fields}); });
                           },
                           [&](LetStmt& l) { check_binding(l, false); },
                           [&](GenesisEntry& ge) {
                               once(genesis_loc, e.loc, "a genesis block");
                               program_labels_.clear();
                               with_ctx(Ctx::genesis, [&] { block(ge.body); });
                           },
                           [&](BootstrapEntry& b) {
                               once(bootstrap_loc, e.loc, "a bootstrap block");
                               with_ctx(Ctx::runtime, [&] { block(b.body); });
                           },
                           [&](ScenarioEntry& s) {
                               auto [it, ok] = scenarios.try_emplace(s.name.name, s.name.loc);
                               if (!ok)
                                   diags_.error(s.name.loc, "duplicate scenario '" + s.name.name + "'")
                                       .notes.push_back({it->second, "previous definition is here"});
                               with_ctx(Ctx::runtime, [&] { block(s.body); });
                           },
                       },
                       e.node);
        }
    }

    template <typename F>
    void with_ctx(Ctx c, F&& f) {
        Ctx saved = ctx_;
        int saved_loops = loop_depth_;
        ctx_ = c;
        loop_depth_ = 0;
        f();
        ctx_ = saved;
        loop_depth_ = saved_loops;
    }

    // ------------------------------------------------------------- types --

    const Type* resolve_type(TypeExpr& t, bool schema_mode) {
        switch (t.kind) {
            case TypeExpr::Kind::list: return T.list(resolve_type(t.args[0], schema_mode));
            case TypeExpr::Kind::array: {
                const Type* elem = resolve_type(t.args[0], schema_mode);
                value_type(*t.size, "array length");
                auto n = const_int(*t.size);
                if (!n || *n < 0) {
                    diags_.error(t.size->loc, "array length must be a non-negative integer literal");
                    return T.list(elem);
                }
                return T.list(elem, static_cast<std::uint64_t>(*n));
            }
            case TypeExpr::Kind::tuple: {
                std::vector<const Type*> elems;
                for (auto& a : t.args) elems.push_back(resolve_type(a, schema_mode));
                if (!schema_mode) {
                    diags_.error(t.loc, "tuple types are only allowed in schema fields");
                    return T.error();
                }
                return T.tuple(std::move(elems));
            }
            case TypeExpr::Kind::named: break;
        }
        static const std::pair<std::string_view, IntWidth> ints[] = {
            {"int", IntWidth::any}, {"u8", IntWidth::u8},   {"u16", IntWidth::u16}, {"u32", IntWidth::u32},
            {"u64", IntWidth::u64}, {"u128", IntWidth::u128}, {"i8", IntWidth::i8}, {"i16", IntWidth::i16},
            {"i32", IntWidth::i32}, {"i64", IntWidth::i64}, {"i128", IntWidth::i128},
        };
        auto no_args = [&](const Type* r) {
            if (!t.args.empty()) diags_.error(t.loc, "type '" + t.name + "' does not take type arguments");
            return r;
        };
        auto not_serializable = [&](const Type* r) {
            if (schema_mode) {
                diags_.error(t.loc, "type '" + t.name + "' cannot be used in a schema (it has no binary encoding)");
                return T.error();
            }
            return r;
        };
        for (auto [name, w] : ints) {
            if (t.name != name) continue;
            if (schema_mode && w == IntWidth::any) {
                diags_.error(t.loc, "schema fields need a sized integer type (u8, u16, u32, u64, u128, i8, ..., i128)");
                return T.error();
            }
            return no_args(T.int_(w));
        }
        if (t.name == "bool") return no_args(T.bool_());
        if (t.name == "float" || t.name == "f64") return no_args(T.float_());
        if (t.name == "f32") return no_args(T.float32());
        if (t.name == "string") return no_args(T.string());
        if (t.name == "bytes") return no_args(T.bytes());
        if (t.name == "pubkey") return no_args(T.pubkey());
        if (t.name == "duration") return not_serializable(no_args(T.duration()));
        if (t.name == "keypair") return not_serializable(no_args(T.keypair()));
        if (t.name == "source") return not_serializable(no_args(T.source()));
        if (t.name == "range") return not_serializable(no_args(T.range()));
        if (t.name == "account_meta") return not_serializable(no_args(T.account_meta()));
        if (t.name == "Option") {
            if (t.args.size() != 1) {
                diags_.error(t.loc, "'Option' takes exactly one type argument");
                return T.error();
            }
            return T.option(resolve_type(t.args[0], schema_mode));
        }
        if (Symbol* s = lookup(t.name); s && s->kind == SymbolKind::schema) return no_args(s->schema_record);
        std::vector<std::string> candidates = {"int", "u8", "u16", "u32", "u64", "u128", "i8", "i16", "i32", "i64",
                                               "i128", "bool", "float", "string", "bytes", "pubkey", "keypair",
                                               "source", "duration", "range", "account_meta", "Option"};
        for (Scope* s = scope_; s; s = s->parent)
            for (const auto& [n, sym] : s->names)
                if (sym->kind == SymbolKind::schema) candidates.push_back(n);
        diags_.error(t.loc, "unknown type '" + t.name + "'" + suggest(t.name, candidates));
        return T.error();
    }

    // ------------------------------------------------- type relationships --

    static bool is_err(const Type* t) { return !t || t->kind == TypeKind::error; }
    static bool is_numeric(const Type* t) { return t->kind == TypeKind::int_ || t->kind == TypeKind::float_; }

    bool assignable(const Type* from, const Type* to) const {
        if (is_err(from) || is_err(to)) return true;
        if (to->kind == TypeKind::any || from->kind == TypeKind::any) return true;
        if (to->kind == TypeKind::option) {
            if (from->kind == TypeKind::null) return true;
            if (from->kind == TypeKind::option) return assignable(from->elem, to->elem);
            return assignable(from, to->elem);
        }
        switch (from->kind) {
            case TypeKind::int_: return to->kind == TypeKind::int_ || to->kind == TypeKind::float_;
            case TypeKind::pubkey: return to->kind == TypeKind::pubkey || to->kind == TypeKind::account_meta;
            case TypeKind::bytes:
                return to->kind == TypeKind::bytes ||
                       (to->kind == TypeKind::list && (to->elem->kind == TypeKind::int_ || to->elem->kind == TypeKind::any));
            case TypeKind::list:
                if (to->kind == TypeKind::bytes) return from->elem->kind == TypeKind::int_ || from->elem->kind == TypeKind::any;
                if (to->kind != TypeKind::list) return false;
                if (to->fixed_len && from->fixed_len && *to->fixed_len != *from->fixed_len) return false;
                return assignable(from->elem, to->elem);
            case TypeKind::record: {
                if (to->kind != TypeKind::record) return false;
                if (to->fields.empty() && !to->schema) return true;  // "any object" (config keys such as invoke args)
                if (to->schema && from->schema) return to->schema == from->schema;
                if (to->fields.size() != from->fields.size()) return false;
                for (const auto& [n, ft] : to->fields) {
                    const Type* f = from->field(n);
                    if (!f || !assignable(f, ft)) return false;
                }
                return true;
            }
            case TypeKind::tuple: {
                if (to->kind != TypeKind::tuple || to->elems.size() != from->elems.size()) return false;
                for (std::size_t i = 0; i < to->elems.size(); ++i)
                    if (!assignable(from->elems[i], to->elems[i])) return false;
                return true;
            }
            case TypeKind::schema: return to->kind == TypeKind::schema && to->schema == from->schema;
            case TypeKind::module: return false;
            default: return from->kind == to->kind;
        }
    }

    // Least upper bound for list-literal inference; nullptr if none.
    const Type* join(const Type* a, const Type* b) {
        if (assignable(b, a) && !(a->kind == TypeKind::int_ && b->kind == TypeKind::float_)) {
            if (a->kind == TypeKind::null) return T.option(b);
            return a;
        }
        if (assignable(a, b)) return b->kind == TypeKind::null ? T.option(a) : b;
        if (a->kind == TypeKind::null) return T.option(b);
        if (b->kind == TypeKind::null) return T.option(a);
        return nullptr;
    }

    // --------------------------------------------------------- constants --

    static std::optional<__int128> const_int(const Expr& e) {
        if (const auto* n = e.as<NumberLit>(); n && n->folded_int && e.type && e.type->kind == TypeKind::int_)
            return n->int_value;
        if (const auto* u = e.as<UnaryExpr>(); u && u->op == UnaryOp::neg)
            if (auto v = const_int(*u->operand)) return -*v;
        return std::nullopt;
    }
    static const std::string* const_str(const Expr& e) {
        if (const auto* s = e.as<StringLit>()) return &s->value;
        return nullptr;
    }
    static BuiltinId const_builtin(const Expr& e) {
        if (const auto* c = e.as<CallExpr>(); c && c->args.empty()) return c->builtin;
        return BuiltinId::none;
    }

    // ------------------------------------------------------- expressions --

    // Checks `e` against `want`, reporting a mismatch. Returns the actual type.
    const Type* expect(Expr& e, const Type* want, const std::string& what) {
        const Type* t = expr(e, want);
        if (t->kind == TypeKind::void_) {
            diags_.error(e.loc, what + " has no value (the expression returns nothing)");
            return T.error();
        }
        if (!assignable(t, want)) {
            diags_.error(e.loc, "type mismatch in " + what + ": expected " + to_string(want) + ", found " + to_string(t));
            return T.error();
        }
        if (want->kind == TypeKind::int_ && want->width != IntWidth::any) {
            if (auto v = const_int(e)) {
                auto [lo, hi] = int_bounds(want->width);
                if (*v < lo || *v > hi)
                    diags_.error(e.loc, "value out of range for " + to_string(want->width) + " in " + what);
            }
        }
        return t;
    }

    // An expression used for its value: must not be void.
    const Type* value_type(Expr& e, const std::string& what) {
        const Type* t = expr(e, nullptr);
        if (t->kind == TypeKind::void_) {
            diags_.error(e.loc, what + " has no value (the expression returns nothing)");
            return T.error();
        }
        return t;
    }

    void expect_source(Expr& e, const std::string& what) {
        const Type* t = value_type(e, what);
        if (!is_err(t) && t->kind != TypeKind::source && t->kind != TypeKind::string)
            diags_.error(e.loc, "type mismatch in " + what + ": expected source (or an RPC URL string), found " + to_string(t));
    }

    const Type* expr(Expr& e, const Type* expected) {
        const Type* t = std::visit([&](auto& n) { return check(e, n, expected); }, e.node);
        e.type = t;
        return t;
    }

    const Type* check(Expr& e, NumberLit& n, const Type* expected) {
        auto parse_int = [&](std::string_view digits, __int128& out) {
            out = 0;
            constexpr __int128 limit = static_cast<__int128>((~static_cast<unsigned __int128>(0)) >> 1);
            for (char c : digits) {
                if (out > (limit - (c - '0')) / 10) return false;
                out = out * 10 + (c - '0');
            }
            return true;
        };
        // Exact decimal scaling: value * 10^scale must be integral.
        auto scaled = [&](int scale, const char* unit_desc) -> std::optional<__int128> {
            auto dot = n.text.find('.');
            std::string whole = n.text.substr(0, dot);
            std::string frac = dot == std::string::npos ? "" : n.text.substr(dot + 1);
            while (!frac.empty() && frac.back() == '0') frac.pop_back();
            if (static_cast<int>(frac.size()) > scale) {
                diags_.error(e.loc, "'" + n.text + "' is not a whole number of " + unit_desc);
                return std::nullopt;
            }
            std::string digits = whole + frac + std::string(static_cast<std::size_t>(scale) - frac.size(), '0');
            __int128 v;
            if (!parse_int(digits, v)) {
                diags_.error(e.loc, "integer literal is too large");
                return std::nullopt;
            }
            return v;
        };
        switch (n.unit) {
            case Unit::none:
                if (n.is_float || (expected && expected->kind == TypeKind::float_)) {
                    n.folded_int = false;
                    n.float_value = std::strtod(n.text.c_str(), nullptr);
                    return T.float_();
                }
                if (auto v = scaled(0, "units")) n.int_value = *v;
                return T.int_();
            case Unit::lamports:
            case Unit::slots:
                if (auto v = scaled(0, n.unit == Unit::lamports ? "lamports" : "slots")) n.int_value = *v;
                return T.int_();
            case Unit::sol:
                if (auto v = scaled(9, "lamports")) n.int_value = *v;
                return T.int_();
            case Unit::ms:
                if (auto v = scaled(0, "milliseconds")) n.int_value = *v;
                return T.duration();
            case Unit::s:
                if (auto v = scaled(3, "milliseconds")) n.int_value = *v;
                return T.duration();
        }
        return T.error();
    }
    const Type* check(Expr&, StringLit&, const Type*) { return T.string(); }
    const Type* check(Expr&, BoolLit&, const Type*) { return T.bool_(); }
    const Type* check(Expr&, NullLit&, const Type*) { return T.null(); }

    const Type* check(Expr& e, IdentExpr& id, const Type*) {
        Symbol* s = lookup(id.name);
        if (!s) {
            if (find_builtin(id.name))
                diags_.error(e.loc, "builtin function '" + id.name + "' must be called, e.g. '" + id.name + "(...)'");
            else
                diags_.error(e.loc, "unknown identifier '" + id.name + "'" + suggest(id.name, visible_names()));
            return T.error();
        }
        id.sym = s;
        if (s->kind == SymbolKind::function) {
            diags_.error(e.loc, "function '" + id.name + "' must be called; functions are not values");
            return T.error();
        }
        return s->type;
    }

    const Type* check(Expr& e, ListLit& l, const Type* expected) {
        if (expected && expected->kind == TypeKind::option) expected = expected->elem;
        if (expected && expected->kind == TypeKind::tuple) {
            if (l.elems.size() != expected->elems.size()) {
                diags_.error(l.elems.empty() ? Loc{} : l.elems.front()->loc,
                             "expected " + std::to_string(expected->elems.size()) + " elements for " + to_string(expected) +
                                 ", found " + std::to_string(l.elems.size()));
                for (auto& el : l.elems) value_type(*el, "list element");
                return T.error();
            }
            for (std::size_t i = 0; i < l.elems.size(); ++i)
                expect(*l.elems[i], expected->elems[i], "tuple element " + std::to_string(i));
            return expected;
        }
        const Type* want = nullptr;
        if (expected && expected->kind == TypeKind::list) want = expected->elem;
        if (expected && expected->kind == TypeKind::bytes) want = T.int_(IntWidth::u8);
        if (want && want->kind != TypeKind::any) {
            for (auto& el : l.elems) expect(*el, want, "list element");
            if (expected->kind == TypeKind::list && expected->fixed_len && l.elems.size() != *expected->fixed_len) {
                diags_.error(e.loc, "expected " + std::to_string(*expected->fixed_len) + " elements for " + to_string(expected) +
                                        ", found " + std::to_string(l.elems.size()));
                return T.error();
            }
            if (expected->kind == TypeKind::bytes) return T.list(T.int_(IntWidth::u8));
            return T.list(want, expected->fixed_len);
        }
        const Type* elem = nullptr;
        for (auto& el : l.elems) {
            const Type* t = value_type(*el, "list element");
            if (is_err(t)) continue;
            if (!elem) {
                elem = t;
                continue;
            }
            const Type* j = join(elem, t);
            if (!j) {
                diags_.error(el->loc, "list elements have incompatible types " + to_string(elem) + " and " + to_string(t));
                return T.error();
            }
            elem = j;
        }
        return T.list(elem ? elem : T.any());
    }

    const Type* check(Expr& e, ObjectLit& o, const Type* expected) {
        if (expected && expected->kind == TypeKind::option) expected = expected->elem;
        std::map<std::string, Loc> seen;
        for (auto& f : o.fields) {
            auto [it, ok] = seen.try_emplace(f.key, f.loc);
            if (!ok)
                diags_.error(f.loc, "duplicate key '" + f.key + "' in object literal")
                    .notes.push_back({it->second, "previous key is here"});
        }
        if (expected && expected->kind == TypeKind::record && !expected->fields.empty()) {
            std::string what = expected->schema ? "schema '" + expected->schema->name + "'" : to_string(expected);
            std::vector<std::string> names;
            for (const auto& [n, _] : expected->fields) names.push_back(n);
            bool ok = true;
            for (auto& f : o.fields) {
                const Type* ft = expected->field(f.key);
                if (!ft) {
                    diags_.error(f.loc, "unknown field '" + f.key + "' for " + what + suggest(f.key, names));
                    value_type(*f.value, "field '" + f.key + "'");
                    ok = false;
                    continue;
                }
                if (is_err(expect(*f.value, ft, "field '" + f.key + "' of " + what))) ok = false;
            }
            for (const auto& [n, ft] : expected->fields) {
                if (!seen.count(n) && ft->kind != TypeKind::option) {
                    diags_.error(e.loc, "missing field '" + n + "' for " + what);
                    ok = false;
                }
            }
            return ok ? expected : T.error();
        }
        std::vector<std::pair<std::string, const Type*>> fields;
        for (auto& f : o.fields) fields.emplace_back(f.key, value_type(*f.value, "field '" + f.key + "'"));
        return T.record(std::move(fields));
    }

    const Type* check(Expr& e, UnaryExpr& u, const Type*) {
        const Type* t = value_type(*u.operand, "operand");
        if (is_err(t)) return t;
        if (u.op == UnaryOp::not_) {
            if (t->kind != TypeKind::bool_) {
                diags_.error(e.loc, "operator '!' expects bool, found " + to_string(t));
                return T.error();
            }
            return t;
        }
        if (t->kind == TypeKind::int_) return T.int_();
        if (t->kind == TypeKind::float_ || t->kind == TypeKind::duration) return t;
        diags_.error(e.loc, "operator '-' expects a number, found " + to_string(t));
        return T.error();
    }

    static const char* op_text(BinaryOp op) {
        switch (op) {
            case BinaryOp::or_: return "||";
            case BinaryOp::and_: return "&&";
            case BinaryOp::eq: return "==";
            case BinaryOp::ne: return "!=";
            case BinaryOp::lt: return "<";
            case BinaryOp::le: return "<=";
            case BinaryOp::gt: return ">";
            case BinaryOp::ge: return ">=";
            case BinaryOp::range: return "..";
            case BinaryOp::range_incl: return "..=";
            case BinaryOp::add: return "+";
            case BinaryOp::sub: return "-";
            case BinaryOp::mul: return "*";
            case BinaryOp::div: return "/";
            case BinaryOp::mod: return "%";
        }
        return "?";
    }

    // Result type of an arithmetic operator; nullptr if not applicable.
    const Type* arith(BinaryOp op, const Type* a, const Type* b) {
        auto k = [](const Type* t) { return t->kind; };
        if (k(a) == TypeKind::int_ && k(b) == TypeKind::int_) return T.int_();
        if (is_numeric(a) && is_numeric(b)) return T.float_();
        if (k(a) == TypeKind::duration && k(b) == TypeKind::duration && (op == BinaryOp::add || op == BinaryOp::sub))
            return T.duration();
        if (k(a) == TypeKind::duration && k(b) == TypeKind::int_ && (op == BinaryOp::mul || op == BinaryOp::div))
            return T.duration();
        if (k(a) == TypeKind::int_ && k(b) == TypeKind::duration && op == BinaryOp::mul) return T.duration();
        if (op == BinaryOp::add) {
            if (k(a) == TypeKind::string && k(b) == TypeKind::string) return T.string();
            if (k(a) == TypeKind::bytes && k(b) == TypeKind::bytes) return T.bytes();
            if (k(a) == TypeKind::list && k(b) == TypeKind::list) {
                if (const Type* j = join(a->elem, b->elem)) return T.list(j);
            }
        }
        return nullptr;
    }

    const Type* check(Expr&, BinaryExpr& b, const Type*) {
        const Type* l = value_type(*b.lhs, "left operand");
        const Type* r = value_type(*b.rhs, "right operand");
        if (is_err(l) || is_err(r)) return T.error();
        auto fail = [&]() {
            diags_.error(b.op_loc, std::string("operator '") + op_text(b.op) + "' cannot be applied to " +
                                       to_string(l) + " and " + to_string(r));
            return T.error();
        };
        switch (b.op) {
            case BinaryOp::or_:
            case BinaryOp::and_:
                if (l->kind != TypeKind::bool_ || r->kind != TypeKind::bool_) return fail();
                return T.bool_();
            case BinaryOp::eq:
            case BinaryOp::ne:
                if (!(assignable(l, r) || assignable(r, l) || (is_numeric(l) && is_numeric(r)))) return fail();
                if (l->kind == TypeKind::keypair || l->kind == TypeKind::module || l->kind == TypeKind::schema) return fail();
                return T.bool_();
            case BinaryOp::lt:
            case BinaryOp::le:
            case BinaryOp::gt:
            case BinaryOp::ge:
                if ((is_numeric(l) && is_numeric(r)) || (l->kind == r->kind && (l->kind == TypeKind::duration ||
                                                                                l->kind == TypeKind::string)))
                    return T.bool_();
                return fail();
            case BinaryOp::range:
            case BinaryOp::range_incl:
                if (l->kind != TypeKind::int_ || r->kind != TypeKind::int_) return fail();
                return T.range();
            default:
                if (const Type* t = arith(b.op, l, r)) return t;
                return fail();
        }
    }

    const Type* check(Expr& e, MemberExpr& m, const Type*) {
        if (auto* id = m.object->as<IdentExpr>()) {
            Symbol* s = lookup(id->name);
            if (s && s->kind == SymbolKind::module) {
                id->sym = s;
                m.object->type = s->type;
                if (!s->module) return T.error();
                auto it = s->module->exports.find(m.member.name);
                if (it == s->module->exports.end()) {
                    std::vector<std::string> names;
                    for (const auto& [n, _] : s->module->exports) names.push_back(n);
                    diags_.error(m.member.loc, "module '" + id->name + "' has no member '" + m.member.name + "'" +
                                                   suggest(m.member.name, names));
                    return T.error();
                }
                m.module_member = it->second;
                if (it->second->kind == SymbolKind::function) {
                    diags_.error(e.loc, "function '" + m.member.name + "' must be called; functions are not values");
                    return T.error();
                }
                return it->second->type;
            }
        }
        const Type* t = value_type(*m.object, "member access");
        if (is_err(t)) return t;
        const std::string& name = m.member.name;
        auto no_member = [&](std::vector<std::string> names = {}) {
            diags_.error(m.member.loc, "type " + to_string(t) + " has no member '" + name + "'" + suggest(name, names));
            return T.error();
        };
        const Type* rec = t->kind == TypeKind::option ? t->elem : t;
        switch (rec->kind) {
            case TypeKind::keypair:
                if (name == "pubkey") return T.pubkey();
                return no_member({"pubkey"});
            case TypeKind::source:
                if (name == "url") return T.string();
                return no_member({"url"});
            case TypeKind::record: {
                if (const Type* f = rec->field(name)) return f;
                std::vector<std::string> names;
                for (const auto& [n, _] : rec->fields) names.push_back(n);
                return no_member(names);
            }
            default: return no_member();
        }
    }

    const Type* check(Expr& e, IndexExpr& ix, const Type*) {
        const Type* t = value_type(*ix.object, "indexed expression");
        expect(*ix.index, T.int_(), "index");
        if (is_err(t)) return t;
        if (t->kind == TypeKind::list) return t->elem->kind == TypeKind::any ? T.error() : t->elem;
        if (t->kind == TypeKind::bytes) return T.int_(IntWidth::u8);
        diags_.error(e.loc, "type " + to_string(t) + " cannot be indexed");
        return T.error();
    }

    const Type* check(Expr& e, CallExpr& c, const Type* expected) {
        // User function, possibly through a module.
        const Symbol* fn = nullptr;
        std::string fn_name;
        if (auto* id = c.callee->as<IdentExpr>()) {
            fn_name = id->name;
            Symbol* s = lookup(id->name);
            if (!s) {
                if (const BuiltinDef* b = find_builtin(id->name)) {
                    c.builtin = b->id;
                    return check_builtin(e, c, *b, expected);
                }
                std::vector<std::string> names;
                for (const auto& b : builtin_table) names.emplace_back(b.name);
                for (Scope* sc = scope_; sc; sc = sc->parent)
                    for (const auto& [n, sym] : sc->names)
                        if (sym->kind == SymbolKind::function) names.push_back(n);
                diags_.error(c.callee->loc, "unknown function '" + id->name + "'" + suggest(id->name, names));
                for (auto& a : c.args) value_type(*a, "argument");
                return T.error();
            }
            id->sym = s;
            if (s->kind != SymbolKind::function) {
                diags_.error(c.callee->loc, "'" + id->name + "' is not a function (it has type " + to_string(s->type) + ")");
                for (auto& a : c.args) value_type(*a, "argument");
                return T.error();
            }
            fn = s;
        } else if (auto* m = c.callee->as<MemberExpr>(); m && m->object->as<IdentExpr>()) {
            auto* id = m->object->as<IdentExpr>();
            Symbol* s = lookup(id->name);
            if (s && s->kind == SymbolKind::module) {
                id->sym = s;
                fn_name = id->name + "." + m->member.name;
                if (!s->module) return T.error();
                auto it = s->module->exports.find(m->member.name);
                if (it == s->module->exports.end() || it->second->kind != SymbolKind::function) {
                    diags_.error(m->member.loc, "module '" + id->name + "' has no function '" + m->member.name + "'");
                    for (auto& a : c.args) value_type(*a, "argument");
                    return T.error();
                }
                m->module_member = it->second;
                fn = it->second;
            }
        }
        if (!fn) {
            value_type(*c.callee, "callee");
            diags_.error(c.callee->loc, "expression is not callable");
            for (auto& a : c.args) value_type(*a, "argument");
            return T.error();
        }
        c.function = fn;
        if (c.args.size() != fn->param_types.size()) {
            diags_.error(e.loc, "function '" + fn_name + "' expects " + std::to_string(fn->param_types.size()) +
                                    " argument(s), found " + std::to_string(c.args.size()))
                .notes.push_back({fn->loc, "'" + fn->name + "' is declared here"});
            for (auto& a : c.args) value_type(*a, "argument");
            return fn->return_type;
        }
        for (std::size_t i = 0; i < c.args.size(); ++i)
            expect(*c.args[i], fn->param_types[i],
                   "argument " + std::to_string(i + 1) + " ('" + fn->function->params[i].name.name + "') of '" + fn_name + "'");
        return fn->return_type;
    }

    // ---------------------------------------------------------- builtins --

    bool arity(Expr& e, CallExpr& c, const BuiltinDef& b, std::size_t min, std::size_t max) {
        if (c.args.size() >= min && c.args.size() <= max) return true;
        std::string expect_s = min == max ? std::to_string(min) : std::to_string(min) + " to " + std::to_string(max);
        diags_.error(e.loc, "'" + std::string(b.name) + "' expects " + expect_s + " argument(s), found " +
                                std::to_string(c.args.size()));
        for (auto& a : c.args) value_type(*a, "argument");
        return false;
    }

    std::string arg_desc(const BuiltinDef& b, std::size_t i) {
        return "argument " + std::to_string(i + 1) + " of '" + std::string(b.name) + "'";
    }

    // Validates a string literal argument with a decoder (pubkey, hex, ...).
    template <typename F>
    void literal_check(Expr& arg, F&& valid, const std::string& msg) {
        if (const std::string* s = const_str(arg); s && !valid(*s)) diags_.error(arg.loc, msg);
    }

    // `pda` seeds: a list whose elements are string, bytes or pubkey.
    void check_seeds(Expr& arg, const BuiltinDef& b) {
        auto seed_ok = [&](Expr& el) {
            const Type* t = value_type(el, "seed");
            if (is_err(t)) return;
            if (t->kind != TypeKind::string && t->kind != TypeKind::bytes && t->kind != TypeKind::pubkey) {
                diags_.error(el.loc, "seeds must be string, bytes or pubkey, found " + to_string(t) +
                                         (t->kind == TypeKind::int_ ? " (use u8(n) or u64_le(n) for integers)" : ""));
                return;
            }
            if (const std::string* s = const_str(el); s && s->size() > sol::max_seed_len)
                diags_.error(el.loc, "seed is " + std::to_string(s->size()) + " bytes; the maximum is 32");
        };
        if (auto* l = arg.as<ListLit>()) {
            if (l->elems.size() > sol::max_seeds - 1)
                diags_.error(arg.loc, "too many seeds (" + std::to_string(l->elems.size()) + "); the maximum is 15 plus the bump");
            for (auto& el : l->elems) seed_ok(*el);
            arg.type = T.list(T.any());
            return;
        }
        const Type* t = value_type(arg, arg_desc(b, 1));
        if (is_err(t)) return;
        if (t->kind != TypeKind::list || (t->elem->kind != TypeKind::string && t->elem->kind != TypeKind::bytes &&
                                          t->elem->kind != TypeKind::pubkey && t->elem->kind != TypeKind::any))
            diags_.error(arg.loc, "type mismatch in " + arg_desc(b, 1) + ": expected a list of seeds (string, bytes or pubkey), found " +
                                      to_string(t));
    }

    // First argument of borsh/anchor/decode: a schema name (possibly module-qualified).
    const Symbol* schema_arg(Expr& arg, const BuiltinDef& b, bool allow_string) {
        const Type* t = value_type(arg, arg_desc(b, 0));
        if (is_err(t)) return nullptr;
        if (t->kind == TypeKind::schema) return t->schema;
        if (allow_string && t->kind == TypeKind::string) {
            if (const std::string* name = const_str(arg)) {
                if (Symbol* s = lookup(*name); s && s->kind == SymbolKind::schema) return s;
            }
            return nullptr;  // resolved at runtime from the program IDL
        }
        diags_.error(arg.loc, "type mismatch in " + arg_desc(b, 0) + ": expected a schema name" +
                                  std::string(allow_string ? " or an account type name string" : "") + ", found " +
                                  to_string(t));
        return nullptr;
    }

    const Type* check_builtin(Expr& e, CallExpr& c, const BuiltinDef& b, const Type* expected) {
        // Effects first: some builtins need network state.
        if (b.effect == Effect::state && ctx_ != Ctx::genesis && ctx_ != Ctx::runtime)
            diags_.error(e.loc, "'" + std::string(b.name) +
                                    "' reads network state and can only be used inside genesis, bootstrap or scenario blocks");
        if (b.effect == Effect::runtime_only && ctx_ != Ctx::runtime)
            diags_.error(e.loc, "'" + std::string(b.name) + "' needs a running network and can only be used inside bootstrap or scenario blocks");

        auto unary = [&](const Type* param, const Type* ret) {
            if (!arity(e, c, b, 1, 1)) return ret;
            expect(*c.args[0], param, arg_desc(b, 0));
            return ret;
        };
        auto nullary = [&](const Type* ret) {
            arity(e, c, b, 0, 0);
            return ret;
        };
        using sol::base58_decode;
        switch (b.id) {
            case BuiltinId::seed:
            case BuiltinId::keypair_file: return unary(T.string(), T.keypair());
            case BuiltinId::pubkey: {
                const Type* r = unary(T.string(), T.pubkey());
                if (c.args.size() == 1)
                    literal_check(*c.args[0], [](const std::string& s) { return sol::Pubkey::from_base58(s).has_value(); },
                                  "invalid pubkey: expected a base58 string encoding 32 bytes");
                return r;
            }
            case BuiltinId::pda:
            case BuiltinId::pda_bump:
                if (arity(e, c, b, 2, 2)) {
                    expect(*c.args[0], T.pubkey(), arg_desc(b, 0) + " (program id)");
                    check_seeds(*c.args[1], b);
                }
                return b.id == BuiltinId::pda ? T.pubkey() : T.int_(IntWidth::u8);
            case BuiltinId::ata:
                if (arity(e, c, b, 2, 3))
                    for (std::size_t i = 0; i < c.args.size(); ++i) expect(*c.args[i], T.pubkey(), arg_desc(b, i));
                return T.pubkey();
            case BuiltinId::system_program:
            case BuiltinId::token_program:
            case BuiltinId::token_2022_program:
            case BuiltinId::associated_token_program:
            case BuiltinId::bpf_loader:
            case BuiltinId::bpf_loader_deprecated:
            case BuiltinId::bpf_upgradeable_loader:
            case BuiltinId::loader_v4:
            case BuiltinId::memo_program:
            case BuiltinId::compute_budget_program:
            case BuiltinId::address_lookup_table_program: return nullary(T.pubkey());
            case BuiltinId::hex: {
                const Type* r = unary(T.string(), T.bytes());
                if (c.args.size() == 1)
                    literal_check(*c.args[0], [](const std::string& s) { return sol::hex_decode(s).has_value(); },
                                  "invalid hex string: expected an even number of hex digits");
                return r;
            }
            case BuiltinId::base64: {
                const Type* r = unary(T.string(), T.bytes());
                if (c.args.size() == 1)
                    literal_check(*c.args[0], [](const std::string& s) { return sol::base64_decode(s).has_value(); },
                                  "invalid base64 string");
                return r;
            }
            case BuiltinId::base58: {
                const Type* r = unary(T.string(), T.bytes());
                if (c.args.size() == 1)
                    literal_check(*c.args[0], [](const std::string& s) { return base58_decode(s).has_value(); },
                                  "invalid base58 string");
                return r;
            }
            case BuiltinId::utf8:
            case BuiltinId::file: return unary(T.string(), T.bytes());
            case BuiltinId::sha256: {
                if (!arity(e, c, b, 1, 1)) return T.bytes();
                const Type* t = value_type(*c.args[0], arg_desc(b, 0));
                if (!is_err(t) && t->kind != TypeKind::string && t->kind != TypeKind::bytes)
                    diags_.error(c.args[0]->loc, "type mismatch in " + arg_desc(b, 0) + ": expected string or bytes, found " + to_string(t));
                return T.bytes();
            }
            case BuiltinId::u8: return unary(T.int_(IntWidth::u8), T.bytes());
            case BuiltinId::u16_le: return unary(T.int_(IntWidth::u16), T.bytes());
            case BuiltinId::u32_le: return unary(T.int_(IntWidth::u32), T.bytes());
            case BuiltinId::u64_le: return unary(T.int_(IntWidth::u64), T.bytes());
            case BuiltinId::i64_le: return unary(T.int_(IntWidth::i64), T.bytes());
            case BuiltinId::borsh:
            case BuiltinId::anchor: {
                if (!arity(e, c, b, 2, 2)) return T.bytes();
                const Symbol* s = schema_arg(*c.args[0], b, b.id == BuiltinId::anchor);
                if (s) expect(*c.args[1], s->schema_record, arg_desc(b, 1));
                else {
                    const Type* t = value_type(*c.args[1], arg_desc(b, 1));
                    if (!is_err(t) && t->kind != TypeKind::record)
                        diags_.error(c.args[1]->loc, "type mismatch in " + arg_desc(b, 1) + ": expected an object, found " + to_string(t));
                }
                return T.bytes();
            }
            case BuiltinId::decode: {
                if (!arity(e, c, b, 2, 2)) return T.error();
                const Symbol* s = schema_arg(*c.args[0], b, false);
                expect(*c.args[1], T.bytes(), arg_desc(b, 1));
                return s ? s->schema_record : T.error();
            }
            case BuiltinId::len: {
                if (!arity(e, c, b, 1, 1)) return T.int_();
                const Type* t = value_type(*c.args[0], arg_desc(b, 0));
                if (!is_err(t) && t->kind != TypeKind::string && t->kind != TypeKind::bytes && t->kind != TypeKind::list)
                    diags_.error(c.args[0]->loc, "'len' expects a string, bytes or list, found " + to_string(t));
                return T.int_();
            }
            case BuiltinId::str:
                if (arity(e, c, b, 1, 1)) value_type(*c.args[0], arg_desc(b, 0));
                return T.string();
            case BuiltinId::print:
                for (auto& a : c.args) value_type(*a, "argument of 'print'");
                return T.void_();
            case BuiltinId::rent_exempt: return unary(T.int_(IntWidth::u64), T.int_());
            case BuiltinId::rpc: return unary(T.string(), T.source());
            case BuiltinId::readonly:
            case BuiltinId::writable: return unary(T.pubkey(), T.account_meta());
            case BuiltinId::signer:
                if (arity(e, c, b, 1, 2)) {
                    expect(*c.args[0], T.pubkey(), arg_desc(b, 0));
                    if (c.args.size() == 2) expect(*c.args[1], T.bool_(), arg_desc(b, 1) + " (writable)");
                }
                return T.account_meta();
            case BuiltinId::sol: {
                if (!arity(e, c, b, 1, 1)) return T.int_();
                const Type* t = value_type(*c.args[0], arg_desc(b, 0));
                if (!is_err(t) && !is_numeric(t))
                    diags_.error(c.args[0]->loc, "type mismatch in " + arg_desc(b, 0) + ": expected a number, found " + to_string(t));
                return T.int_();
            }
            case BuiltinId::system_transfer:
            case BuiltinId::spl_transfer:
            case BuiltinId::spl_mint_to: return unary(T.int_(IntWidth::u64), T.bytes());
            case BuiltinId::spl_transfer_checked:
                if (arity(e, c, b, 2, 2)) {
                    expect(*c.args[0], T.int_(IntWidth::u64), arg_desc(b, 0) + " (amount)");
                    expect(*c.args[1], T.int_(IntWidth::u8), arg_desc(b, 1) + " (decimals)");
                }
                return T.bytes();
            case BuiltinId::memo: return unary(T.string(), T.bytes());
            case BuiltinId::balance:
            case BuiltinId::token_balance: return unary(T.pubkey(), T.int_());
            case BuiltinId::account: return unary(T.pubkey(), T.option(T.account_info()));
            case BuiltinId::exists: return unary(T.pubkey(), T.bool_());
            case BuiltinId::slot:
            case BuiltinId::epoch: return nullary(T.int_());
            case BuiltinId::none: break;
        }
        (void)expected;
        return T.error();
    }

    // -------------------------------------------------------- statements --

    void block(Block& b) {
        ScopeGuard g(*this);
        for (auto& s : b) stmt(*s);
    }

    void loop_body(Block& b, ForStmt* f, const Type* var_type) {
        ScopeGuard g(*this);
        if (f) f->sym = declare(SymbolKind::loop_var, f->var, var_type);
        ++loop_depth_;
        for (auto& s : b) stmt(*s);
        --loop_depth_;
    }

    static const char* ctx_name(Ctx c) {
        switch (c) {
            case Ctx::top_level: return "at top level";
            case Ctx::function: return "in a function";
            case Ctx::config: return "in network configuration";
            case Ctx::genesis: return "in genesis";
            case Ctx::runtime: return "in bootstrap/scenario";
        }
        return "";
    }

    bool require_genesis(const Stmt& s, const char* what) {
        if (ctx_ == Ctx::genesis) return true;
        diags_.error(s.loc, std::string("'") + what + "' declarations are only allowed inside genesis (found " +
                                ctx_name(ctx_) + ")");
        return false;
    }

    bool require_runtime(const Stmt& s, const char* what) {
        if (ctx_ == Ctx::runtime) return true;
        std::string hint = ctx_ == Ctx::genesis ? "; genesis builds state directly, use declarations such as 'account' instead" : "";
        diags_.error(s.loc, std::string("'") + what + "' is a runtime statement and is only allowed inside bootstrap or scenario (found " +
                                ctx_name(ctx_) + ")" + hint);
        return false;
    }

    void reserved_feature(const Stmt& s, const char* what) {
        diags_.error(s.loc, std::string("'") + what +
                                "' is reserved for a future version of Tide: live-network control (warp, snapshot, restore) is not supported yet");
    }

    void stmt(Stmt& s) {
        std::visit(overloaded{
                       [&](LetStmt& l) {
                           if (l.kind != BindingKind::let)
                               diags_.error(s.loc, std::string("'") + (l.kind == BindingKind::keypair ? "keypair" : "source") +
                                                       "' declarations are only allowed at top level or directly inside a network");
                           check_binding(l, false);
                       },
                       [&](AssignStmt& a) { check_assign(a); },
                       [&](IfStmt& i) {
                           expect(*i.cond, T.bool_(), "if condition");
                           block(i.then_block);
                           if (i.else_block) block(*i.else_block);
                       },
                       [&](ForStmt& f) {
                           const Type* t = value_type(*f.iterable, "for loop");
                           const Type* var = T.error();
                           if (t->kind == TypeKind::range) var = T.int_();
                           else if (t->kind == TypeKind::list) var = t->elem;
                           else if (!is_err(t))
                               diags_.error(f.iterable->loc, "cannot iterate over " + to_string(t) + "; expected a range or a list");
                           loop_body(f.body, &f, var);
                       },
                       [&](WhileStmt& w) {
                           expect(*w.cond, T.bool_(), "while condition");
                           loop_body(w.body, nullptr, nullptr);
                       },
                       [&](AssertStmt& a) { expect(*a.cond, T.bool_(), "assertion"); },
                       [&](ExprStmt& es) {
                           const Type* t = expr(*es.expr, nullptr);
                           if (!es.expr->as<CallExpr>() && !is_err(t))
                               diags_.warning(es.expr->loc, "expression result is unused");
                       },
                       [&](BreakStmt&) {
                           if (loop_depth_ == 0) diags_.error(s.loc, "'break' outside of a loop");
                       },
                       [&](ContinueStmt&) {
                           if (loop_depth_ == 0) diags_.error(s.loc, "'continue' outside of a loop");
                       },
                       [&](ReturnStmt& r) { check_return(s, r); },
                       [&](EntityDecl& d) { check_entity(s, d); },
                       [&](SysvarDecl& d) {
                           require_genesis(s, "sysvar");
                           const SysvarSpec* spec = nullptr;
                           for (const auto& sv : sysvar_specs)
                               if (sv.name == d.name.name) spec = &sv;
                           if (!spec) {
                               std::vector<std::string> names;
                               for (const auto& sv : sysvar_specs) names.emplace_back(sv.name);
                               diags_.error(d.name.loc, "unknown or unsupported sysvar '" + d.name.name + "'" + suggest(d.name.name, names));
                               return;
                           }
                           check_block(d.body, {spec->name, spec->fields});
                       },
                       [&](FeatureDecl& f) {
                           require_genesis(s, "feature");
                           expect(*f.feature, T.pubkey(), "feature id");
                       },
                       [&](ForkDecl& f) {
                           require_genesis(s, "fork");
                           expect_source(*f.source, "fork source");
                           if (f.at_slot) expect(*f.at_slot, T.int_(IntWidth::u64), "fork slot");
                       },
                       [&](CloneDecl& cl) {
                           require_genesis(s, "clone");
                           expect(*cl.target, T.pubkey(), "clone target");
                           expect_source(*cl.source, "clone source");
                           if (cl.at_slot) expect(*cl.at_slot, T.int_(IntWidth::u64), "clone slot");
                           if (cl.overrides) {
                               if (cl.program) diags_.error(cl.overrides->loc, "overrides are only supported when cloning accounts, not programs");
                               check_block(*cl.overrides, {"clone overrides", clone_override_fields});
                           }
                       },
                       [&](DeleteDecl& d) {
                           require_genesis(s, "delete");
                           expect(*d.target, T.pubkey(), "delete target");
                       },
                       [&](TransactionStmt& t) { check_transaction(s, t); },
                       [&](InvokeStmt& i) {
                           require_runtime(s, "invoke");
                           expect(*i.program, T.pubkey(), "invoked program");
                           check_block(i.with, {"invoke", invoke_fields});
                       },
                       [&](DeployStmt& d) {
                           require_runtime(s, "deploy");
                           expect(*d.program, T.keypair(), "deployed program (its keypair is the program id)");
                           expect(*d.from, T.string(), "program binary path");
                       },
                       [&](AirdropStmt& a) {
                           require_runtime(s, "airdrop");
                           expect(*a.to, T.pubkey(), "airdrop recipient");
                           expect(*a.amount, T.int_(IntWidth::u64), "airdrop amount");
                       },
                       [&](WarpStmt& w) {
                           if (!require_runtime(s, "warp")) return;
                           if (w.kind == WarpKind::to_time) {
                               const Type* t = value_type(*w.value, "warp time");
                               (void)t;
                           } else {
                               expect(*w.value, T.int_(IntWidth::u64), "warp target");
                           }
                           reserved_feature(s, "warp");
                       },
                       [&](SnapshotStmt& sn) {
                           if (!require_runtime(s, "snapshot")) return;
                           expect(*sn.name, T.string(), "snapshot name");
                           reserved_feature(s, "snapshot");
                       },
                       [&](RestoreStmt& r) {
                           if (!require_runtime(s, "restore")) return;
                           expect(*r.name, T.string(), "snapshot name");
                           reserved_feature(s, "restore");
                       },
                       [&](AwaitStmt& a) {
                           require_runtime(s, "await");
                           expect(*a.cond, T.bool_(), "await condition");
                       },
                   },
                   s.node);
    }

    // Validates an lvalue; returns its type or nullptr after reporting.
    const Type* lvalue(Expr& e) {
        if (auto* id = e.as<IdentExpr>()) {
            Symbol* s = lookup(id->name);
            if (!s) {
                diags_.error(e.loc, "unknown identifier '" + id->name + "'" + suggest(id->name, visible_names()));
                return nullptr;
            }
            id->sym = s;
            e.type = s->type;
            if (!s->is_mutable) {
                static const char* kinds[] = {"variable", "keypair", "source", "function", "schema", "module",
                                              "parameter", "loop variable", "transaction result"};
                diags_.error(e.loc, std::string("cannot assign to ") + kinds[static_cast<int>(s->kind)] + " '" + id->name +
                                        "'; only 'let' variables are mutable")
                    .notes.push_back({s->loc, "declared here"});
                return nullptr;
            }
            if (s->is_global && ctx_ != Ctx::top_level) {
                diags_.error(e.loc, "cannot assign to top-level variable '" + id->name + "' here; top-level bindings are immutable outside top-level code")
                    .notes.push_back({s->loc, "declared here"});
                return nullptr;
            }
            return s->type;
        }
        if (auto* m = e.as<MemberExpr>()) {
            const Type* t = lvalue(*m->object);
            if (!t) return nullptr;
            if (t->kind != TypeKind::record) {
                diags_.error(m->member.loc, "cannot assign to member '" + m->member.name + "' of " + to_string(t));
                return nullptr;
            }
            const Type* f = t->field(m->member.name);
            if (!f) {
                diags_.error(m->member.loc, "type " + to_string(t) + " has no member '" + m->member.name + "'");
                return nullptr;
            }
            e.type = f;
            return f;
        }
        if (auto* ix = e.as<IndexExpr>()) {
            const Type* t = lvalue(*ix->object);
            expect(*ix->index, T.int_(), "index");
            if (!t) return nullptr;
            const Type* r = nullptr;
            if (t->kind == TypeKind::list) r = t->elem;
            else if (t->kind == TypeKind::bytes) r = T.int_(IntWidth::u8);
            else {
                diags_.error(e.loc, "type " + to_string(t) + " cannot be indexed");
                return nullptr;
            }
            e.type = r;
            return r;
        }
        diags_.error(e.loc, "invalid assignment target; expected a variable, a field or an index expression");
        return nullptr;
    }

    void check_assign(AssignStmt& a) {
        const Type* target = lvalue(*a.target);
        if (!target) {
            value_type(*a.value, "assigned value");
            return;
        }
        if (a.op == AssignOp::set) {
            expect(*a.value, target, "assignment");
            return;
        }
        const Type* v = value_type(*a.value, "assigned value");
        if (is_err(v) || is_err(target)) return;
        static const BinaryOp ops[] = {BinaryOp::add, BinaryOp::add, BinaryOp::sub, BinaryOp::mul, BinaryOp::div, BinaryOp::mod};
        const Type* r = arith(ops[static_cast<int>(a.op)], target, v);
        if (!r || !assignable(r, target))
            diags_.error(a.value->loc, "compound assignment cannot combine " + to_string(target) + " with " + to_string(v));
    }

    void check_return(Stmt& s, ReturnStmt& r) {
        if (ctx_ != Ctx::function || !function_) {
            diags_.error(s.loc, std::string("'return' outside of a function (found ") + ctx_name(ctx_) + ")");
            if (r.value) value_type(*r.value, "return value");
            return;
        }
        const Type* want = function_->return_type;
        if (want->kind == TypeKind::void_) {
            if (r.value) {
                diags_.error(r.value->loc, "function '" + function_->name + "' does not declare a return type and cannot return a value");
                value_type(*r.value, "return value");
            }
            return;
        }
        if (!r.value) {
            diags_.error(s.loc, "function '" + function_->name + "' must return a value of type " + to_string(want));
            return;
        }
        expect(*r.value, want, "return value of '" + function_->name + "'");
    }

    void check_entity(Stmt& s, EntityDecl& d) {
        static const char* names[] = {"account", "program", "mint", "token_account", "associated_token_account", "lookup_table"};
        const char* what = names[static_cast<int>(d.kind)];
        require_genesis(s, what);
        if (d.target) expect(*d.target, T.pubkey(), std::string(what) + " address");
        BlockSpec spec{what, {}};
        switch (d.kind) {
            case EntityKind::account: spec.fields = account_fields; break;
            case EntityKind::program:
                spec.fields = program_fields;
                if (auto [it, ok] = program_labels_.try_emplace(d.label.name, d.label.loc); !ok)
                    diags_.error(d.label.loc, "duplicate program '" + d.label.name + "' in genesis")
                        .notes.push_back({it->second, "previous definition is here"});
                break;
            case EntityKind::mint:
                spec.fields = mint_fields;
                spec.extensions = mint_extensions;
                break;
            case EntityKind::token_account:
                spec.fields = token_account_fields;
                spec.extensions = token_account_extensions;
                break;
            case EntityKind::associated_token_account: spec.fields = ata_fields; break;
            case EntityKind::lookup_table: spec.fields = lookup_table_fields; break;
        }
        check_entries(d.fields, spec, s.loc);
        // Static lints over literal values.
        auto field = [&](std::string_view n) -> Expr* {
            for (auto& e : d.fields)
                if (e.kind == RecordEntry::Kind::value && e.key == n) return e.value.get();
            return nullptr;
        };
        if (d.kind == EntityKind::token_account) {
            if (Expr* st = field("state"))
                literal_check(*st, [](const std::string& v) { return v == "initialized" || v == "frozen"; },
                              "token account state must be \"initialized\" or \"frozen\"");
        }
        if (d.kind == EntityKind::account) {
            Expr* exe = field("executable");
            Expr* owner = field("owner");
            bool exec_true = exe && exe->as<BoolLit>() && exe->as<BoolLit>()->value;
            if (exec_true && (!owner || const_builtin(*owner) == BuiltinId::system_program))
                diags_.warning(exe->loc, "executable account is owned by the system program; executable accounts are normally owned by a loader");
        }
        for (auto& e : d.fields) {
            if (e.kind != RecordEntry::Kind::extension) continue;
            for (auto& x : e.block->entries)
                if (x.key == "state" && x.value)
                    literal_check(*x.value, [](const std::string& v) { return v == "initialized" || v == "frozen"; },
                                  "default account state must be \"initialized\" or \"frozen\"");
        }
    }

    void check_transaction(Stmt& s, TransactionStmt& t) {
        require_runtime(s, "transaction");
        std::map<std::string, Loc> seen;
        std::vector<std::string> keys;
        for (const auto& f : tx_fields) keys.emplace_back(f.name);
        std::size_t instructions = 0;
        for (auto& e : t.entries) {
            if (e.is_instruction) {
                ++instructions;
                expect(*e.program, T.pubkey(), "instruction program id");
                check_block(e.body, {"instruction", instruction_fields});
                continue;
            }
            auto [it, ok] = seen.try_emplace(e.key, e.loc);
            if (!ok)
                diags_.error(e.loc, "duplicate transaction field '" + e.key + "'").notes.push_back({it->second, "previous value is here"});
            const FieldSpec* spec = nullptr;
            for (const auto& f : tx_fields)
                if (f.name == e.key) spec = &f;
            if (!spec) {
                diags_.error(e.loc, "unknown transaction field '" + e.key + "'" + suggest(e.key, keys));
                value_type(*e.value, "field '" + e.key + "'");
                continue;
            }
            expect(*e.value, spec_type(spec->type), "transaction field '" + e.key + "'");
        }
        if (instructions == 0) diags_.error(s.loc, "transaction has no instructions");
        if (t.binding) {
            t.sym = declare(SymbolKind::tx_binding, *t.binding, T.tx_result());
        }
    }

    // ----------------------------------------------------- record blocks --

    const Type* spec_type(Ty t) {
        switch (t) {
            case Ty::any: return T.any();
            case Ty::int_: return T.int_();
            case Ty::u8: return T.int_(IntWidth::u8);
            case Ty::u16: return T.int_(IntWidth::u16);
            case Ty::u32: return T.int_(IntWidth::u32);
            case Ty::u64: return T.int_(IntWidth::u64);
            case Ty::float_:
            case Ty::number: return T.float_();
            case Ty::bool_: return T.bool_();
            case Ty::string: return T.string();
            case Ty::bytes: return T.bytes();
            case Ty::pubkey: return T.pubkey();
            case Ty::opt_pubkey: return T.option(T.pubkey());
            case Ty::keypair: return T.keypair();
            case Ty::source: return T.source();
            case Ty::duration: return T.duration();
            case Ty::list_pubkey: return T.list(T.pubkey());
            case Ty::list_keypair: return T.list(T.keypair());
            case Ty::list_meta: return T.list(T.account_meta());
            case Ty::record: return T.record({});
        }
        return T.any();
    }

    void check_block(RecordBlock& b, const BlockSpec& spec) { check_entries(b.entries, spec, b.loc); }

    void check_entries(std::vector<RecordEntry>& entries, const BlockSpec& spec, Loc block_loc) {
        std::map<std::string, Loc> seen;
        std::vector<std::string> names;
        for (const auto& f : spec.fields) names.emplace_back(f.name);
        for (auto& e : entries) {
            if (e.kind == RecordEntry::Kind::extension) {
                if (spec.extensions.empty()) {
                    diags_.error(e.loc, std::string("extensions are not supported in ") + std::string(spec.what));
                    continue;
                }
                const ExtSpec* ext = nullptr;
                std::vector<std::string> ext_names;
                for (const auto& x : spec.extensions) {
                    ext_names.emplace_back(x.name);
                    if (x.name == e.key) ext = &x;
                }
                if (!ext) {
                    diags_.error(e.loc, "unknown " + std::string(spec.what) + " extension '" + e.key + "'" + suggest(e.key, ext_names));
                    continue;
                }
                auto [it, ok] = seen.try_emplace("extension " + e.key, e.loc);
                if (!ok) diags_.error(e.loc, "duplicate extension '" + e.key + "'").notes.push_back({it->second, "previous definition is here"});
                check_entries(e.block->entries, {ext->name, ext->fields}, e.block->loc);
                continue;
            }
            auto [it, ok] = seen.try_emplace(e.key, e.loc);
            if (!ok)
                diags_.error(e.loc, "duplicate field '" + e.key + "' in " + std::string(spec.what))
                    .notes.push_back({it->second, "previous value is here"});
            const FieldSpec* fs = nullptr;
            for (const auto& f : spec.fields)
                if (f.name == e.key) fs = &f;
            if (!fs) {
                diags_.error(e.loc, "unknown field '" + e.key + "' in " + std::string(spec.what) + suggest(e.key, names));
                if (e.value) value_type(*e.value, "field '" + e.key + "'");
                continue;
            }
            if (e.kind == RecordEntry::Kind::nested) {
                diags_.error(e.loc, "field '" + e.key + "' expects a value: write '" + e.key + " = ...;'");
                continue;
            }
            std::string what = "field '" + e.key + "' of " + std::string(spec.what);
            if (fs->type == Ty::source) expect_source(*e.value, what);
            else if (fs->type == Ty::number) {
                const Type* t = value_type(*e.value, what);
                if (!is_err(t) && !is_numeric(t))
                    diags_.error(e.value->loc, "type mismatch in " + what + ": expected a number, found " + to_string(t));
            } else expect(*e.value, spec_type(fs->type), what);
        }
        for (const auto& f : spec.fields)
            if (f.required && !seen.count(std::string(f.name)))
                diags_.error(block_loc, "missing required field '" + std::string(f.name) + "' in " + std::string(spec.what));
    }

    Program& prog_;
    TypeContext& T;
    SourceManager& sm_;
    Diagnostics& diags_;
    Module* module_ = nullptr;
    Scope* scope_ = nullptr;
    Ctx ctx_ = Ctx::top_level;
    int loop_depth_ = 0;
    const Symbol* function_ = nullptr;
    std::map<std::string, Loc> program_labels_;
    std::map<std::filesystem::path, Module*> loaded_;
    std::set<std::filesystem::path> in_progress_;
};

}  // namespace

std::unique_ptr<Program> analyze(Document& root, SourceManager& sources, Diagnostics& diags) {
    auto prog = std::make_unique<Program>();
    std::size_t before = diags.error_count();
    Checker checker(*prog, sources, diags);
    checker.load_root(root);
    // The root module must come last in initialization order; check_module
    // appended it after its imports already.
    if (diags.error_count() != before) return nullptr;
    return prog;
}

}  // namespace tide::sema
